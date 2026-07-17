#!/usr/bin/env python3
"""Surround Part B — one-shot GDB-RSP reader for the DSP per-voice quad gain
matrix `SourceConfiguration.gain[3][4]`.

Binds the NW4C `span` (0xD7) command to its cause. While the Part B stimulus
cartridge (`build_partb_cartridge.py`) holds a note and sweeps `span`, this
client attaches to StreetPass Mii Plaza over the console's **Luma3DS Rosalina
GDB stub** (RSP over TCP), reads the live `gain[3][4]` floats out of the DSP
shared region mapped in the game's address space, and prints them labelled by
mixer/channel. A one-shot ALL-STOP snapshot: attaching halts the ARM11 process,
so the config is frozen at its last-written state (which reflects the span value
active at that instant), we read, then detach and the game resumes.

The DSP shared-memory addressing is IDENTICAL to the in-process dsp-tap plugin
(tools/dsp-tap): the GDB `m<addr>,<len>` packet reads the *debuggee's* virtual
memory, and the DSP RAM is mapped at the same fixed VA (0x1FF00000, data at
0x1FF40000) in MiiPlaza's process — so the same address arithmetic applies.

The gain values are **32-bit IEEE-754 little-endian floats** (NOT fixed-point):
`SourceConfiguration::gain` is `float_le gain[3][4]` at slot offset 0x04
(tools/dsp-oracle/src/shared_mem.h `SrcCfgOffset::gain`, Azahar
shared_memory.h). 3 mixers x 4 channels = 12 floats = 48 bytes.

The RSP framing + hex decode are unit-tested offline against a fake stub
(`test_gdb_read.py`) — no console needed to verify the packet layer.

Stdlib only. Read-only: the client never writes console memory.
"""

import argparse
import socket
import struct
import sys

# ---------------------------------------------------------------------------
# DSP shared-memory layout — from tools/dsp-tap/include/dsp_regions.h and
# tools/dsp-oracle/src/shared_mem.h. WORD addresses are firmware-specific; these
# are MiiPlaza's (dspfirm.cdc sha prefix 944b40b5), the same exemplar values the
# oracle and the dsp-tap plugin use.
# ---------------------------------------------------------------------------
DSP_DATA_VADDR = 0x1FF40000        # DSP_RAM_VADDR (0x1FF00000) + 0x40000
REGION1_WORD_OR = 0x10000          # region-1 word = region-0 word | 0x10000

WORD_FRAME_COUNTER = 0xBFFF        # u16 bank selector (last word of a bank)
WORD_SOURCE_CONFIG = 0x9E92        # SourceConfiguration::Configuration[24]
WORD_DSP_CONFIG = 0x9430           # DspConfiguration, 196 B

# SourceConfiguration::Configuration slot layout (192 B/source):
SRC_CFG_STRIDE = 192
NUM_SOURCES = 24
GAIN_OFF = 0x04                    # float_le gain[3][4]
GAIN_COUNT = 12                    # 3 mixers x 4 channels
GAIN_BYTES = GAIN_COUNT * 4        # 48
ENABLE_OFF = 0xA0                  # u8 (1 = voice active)

# DspConfiguration fields used as an in-band cross-check (surround only lifts
# the rear lanes when the console is actually in Surround output mode):
DSP_CFG_OUTPUT_FORMAT_OFF = 0x16   # u16 OutputFormat
DSP_CFG_SURROUND_DEPTH_OFF = 0x1C  # float_le
OUTPUT_FORMAT_NAMES = {0: "Mono", 1: "Stereo", 2: "Surround"}

# gain[mixer][channel] labels. mixer 0 = MAIN (feeds the DAC), 1/2 = aux/effect
# buses. channel 0/1 = front L/R, 2/3 = rear L/R (the span axis lives here).
MIXER_NAMES = ("MAIN", "AUX-A(reverb)", "AUX-B(chorus)")
CHANNEL_NAMES = ("frontL", "frontR", "rearL ", "rearR ")


def word_vaddr(word, region=0):
    """ARM11 virtual address of a DSP data WORD in the given bank (0 or 1)."""
    w = word | (REGION1_WORD_OR if region else 0)
    return DSP_DATA_VADDR + w * 2


# ===========================================================================
# RSP (GDB Remote Serial Protocol) framing — the unit-tested core
# ===========================================================================

def rsp_checksum(payload: bytes) -> int:
    """RSP packet checksum: sum of the (transmitted) payload bytes mod 256."""
    return sum(payload) & 0xFF


def build_packet(payload: bytes) -> bytes:
    """Frame a payload as `$<payload>#<2-hex-checksum>`."""
    return b"$" + payload + b"#" + ("%02x" % rsp_checksum(payload)).encode("ascii")


def rsp_decode_body(payload: bytes) -> bytes:
    """Decode an RSP packet body: undo `}` escaping (next byte XOR 0x20) and `*`
    run-length encoding (preceding char repeated count-29 extra times). The
    checksum is over the RAW (still-encoded) body, so callers verify the sum
    BEFORE calling this."""
    out = bytearray()
    i = 0
    while i < len(payload):
        c = payload[i]
        if c == 0x7D:                     # '}' escape
            out.append(payload[i + 1] ^ 0x20)
            i += 2
        elif c == 0x2A and out:           # '*' run-length
            out.extend([out[-1]] * (payload[i + 1] - 29))
            i += 2
        else:
            out.append(c)
            i += 1
    return bytes(out)


class RspConnection:
    """Minimal RSP client over a connected socket. Handles ack framing; the
    Luma stub uses acks by default (no QStartNoAckMode negotiated here)."""

    def __init__(self, sock, use_ack=True):
        self.sock = sock
        self.use_ack = use_ack
        self._buf = bytearray()

    def _fill(self):
        chunk = self.sock.recv(8192)
        if not chunk:
            raise ConnectionError("GDB stub closed the connection")
        self._buf += chunk

    def _read_byte(self) -> int:
        while not self._buf:
            self._fill()
        b = self._buf[0]
        del self._buf[0]
        return b

    def send_packet(self, payload: bytes):
        pkt = build_packet(payload)
        for _ in range(6):
            self.sock.sendall(pkt)
            if not self.use_ack:
                return
            b = self._read_byte()
            while b not in (0x2B, 0x2D):   # skip noise until '+'/'-'
                b = self._read_byte()
            if b == 0x2B:                  # '+' acknowledged
                return
        raise IOError("no ACK from stub after 6 retries")

    def recv_packet(self) -> bytes:
        while self._read_byte() != 0x24:   # scan to '$'
            pass
        body = bytearray()
        b = self._read_byte()
        while b != 0x23:                   # up to '#'
            body.append(b)
            b = self._read_byte()
        csum = bytes([self._read_byte(), self._read_byte()])
        if int(csum, 16) != rsp_checksum(bytes(body)):
            if self.use_ack:
                self.sock.sendall(b"-")
            raise IOError("RSP checksum mismatch")
        if self.use_ack:
            self.sock.sendall(b"+")
        return rsp_decode_body(bytes(body))


# ===========================================================================
# Handshake + memory reads
# ===========================================================================

def handshake(conn: RspConnection):
    """qSupported to learn PacketSize, then `?` to confirm the target is
    halted (a selected/attached Luma process stops on connect). Returns
    (max_packet_bytes, stop_reason_payload)."""
    conn.send_packet(b"qSupported:swbreak+;hwbreak+;")
    reply = conn.recv_packet()
    max_pkt = 0x400
    for field in reply.split(b";"):
        if field.startswith(b"PacketSize="):
            try:
                max_pkt = int(field[len(b"PacketSize="):], 16)
            except ValueError:
                pass
    conn.send_packet(b"?")
    stop = conn.recv_packet()
    return max_pkt, stop


def read_mem(conn: RspConnection, addr: int, length: int, max_pkt: int = 0x400) -> bytes:
    """Read `length` bytes at `addr` from the debuggee via `m<addr>,<len>`,
    chunked to stay under the stub's packet size, hex-decoding each reply."""
    out = bytearray()
    per = max(1, min(length, (max_pkt - 8) // 2))   # 2 hex chars/byte + framing
    off = 0
    while off < length:
        n = min(per, length - off)
        conn.send_packet(b"m%x,%x" % (addr + off, n))
        reply = conn.recv_packet()
        if reply[:1] == b"E" and len(reply) == 3:
            raise IOError("memory read error %s at %#x"
                          % (reply.decode("ascii"), addr + off))
        out += bytes.fromhex(reply.decode("ascii"))
        off += n
    return bytes(out)


def read_u16(conn, addr, max_pkt=0x400):
    return struct.unpack("<H", read_mem(conn, addr, 2, max_pkt))[0]


def counter_newer(a: int, b: int) -> bool:
    """True if u16 frame counter a is newer than b, with signed wrap
    (int16(a-b) > 0) — matches the oracle's CurrentRegionIndex."""
    d = (a - b) & 0xFFFF
    return d != 0 and (d & 0x8000) == 0


def current_region(conn, max_pkt=0x400):
    """Pick the CURRENT (higher-counter) bank — SourceConfiguration and
    DspConfiguration are application-written inputs, read from the bank the DSP
    is acting on now. Returns (region, counter0, counter1)."""
    c0 = read_u16(conn, word_vaddr(WORD_FRAME_COUNTER, 0), max_pkt)
    c1 = read_u16(conn, word_vaddr(WORD_FRAME_COUNTER, 1), max_pkt)
    return (1 if counter_newer(c1, c0) else 0), c0, c1


def parse_voices(raw: bytes):
    """Split a SourceConfiguration array blob into (index, enable, gains[12])."""
    voices = []
    for v in range(NUM_SOURCES):
        slot = raw[v * SRC_CFG_STRIDE:(v + 1) * SRC_CFG_STRIDE]
        enable = slot[ENABLE_OFF]
        gains = struct.unpack("<%df" % GAIN_COUNT,
                              slot[GAIN_OFF:GAIN_OFF + GAIN_BYTES])
        voices.append((v, enable, gains))
    return voices


def snapshot(conn, region, max_pkt=0x400, source_config_word=WORD_SOURCE_CONFIG):
    """One consistent read (target is halted): all 24 voice configs + the
    DspConfiguration output-format cross-check, from the given bank."""
    base = word_vaddr(source_config_word, region)
    raw = read_mem(conn, base, SRC_CFG_STRIDE * NUM_SOURCES, max_pkt)
    voices = parse_voices(raw)
    of_addr = word_vaddr(WORD_DSP_CONFIG, region) + DSP_CFG_OUTPUT_FORMAT_OFF
    output_format = struct.unpack("<H", read_mem(conn, of_addr, 2, max_pkt))[0]
    sd_addr = word_vaddr(WORD_DSP_CONFIG, region) + DSP_CFG_SURROUND_DEPTH_OFF
    surround_depth = struct.unpack("<f", read_mem(conn, sd_addr, 4, max_pkt))[0]
    return voices, output_format, surround_depth


def detach(conn):
    """Resume the process and drop the debug session (audio resumes)."""
    try:
        conn.send_packet(b"D")
        conn.recv_packet()
    except (IOError, ConnectionError):
        pass


# ===========================================================================
# Reporting
# ===========================================================================

def format_gain_matrix(gains):
    lines = []
    for m in range(3):
        row = gains[m * 4:m * 4 + 4]
        cells = "  ".join("%s=%+.4f" % (CHANNEL_NAMES[c], row[c]) for c in range(4))
        lines.append("      [%-13s] %s" % (MIXER_NAMES[m], cells))
    return "\n".join(lines)


def print_report(voices, output_format, surround_depth, region, c0, c1,
                 label, show_all):
    print("=" * 72)
    if label is not None:
        print("snapshot label (operator-declared active value): %s" % label)
    of_name = OUTPUT_FORMAT_NAMES.get(output_format, "?")
    print("DspConfiguration.output_format = %d (%s)   surround_depth = %.4f"
          % (output_format, of_name, surround_depth))
    print("frame counters: region0=%d region1=%d -> current bank = region %d"
          % (c0, c1, region))
    if of_name == "Surround":
        print("  [OK] console IS in Surround output mode -> rear lanes should "
              "track span")
    else:
        print("  [cross-check] output mode is %s -> MAIN rear lanes "
              "gain[0][2],[0][3] should be ~0 regardless of span" % of_name)
    print("-" * 72)

    active = [v for v in voices if v[1] != 0]
    shown = voices if show_all else (active if active else voices[:1])
    if not active:
        print("WARNING: no voice has enable!=0 — is a note actually sounding? "
              "(showing voice 0)")
    for v, enable, gains in shown:
        tag = "ACTIVE" if enable else "idle"
        rearL, rearR = gains[2], gains[3]
        note = ""
        if enable:
            note = "  <-- MAIN rear = (%+.4f, %+.4f)" % (rearL, rearR)
        print("voice %2d  enable=%d (%s)%s" % (v, enable, tag, note))
        print(format_gain_matrix(gains))
    print("=" * 72)
    if active:
        print("Identify the stimulus voice = the one that stays ACTIVE across "
              "snapshots and whose MAIN front/rear balance shifts with span.")


# ===========================================================================
# CLI
# ===========================================================================

def run(host, port, label, show_all, source_config_word, use_ack, timeout):
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        conn = RspConnection(sock, use_ack=use_ack)
        max_pkt, stop = handshake(conn)
        print("connected %s:%d  PacketSize=%#x  stop=%s"
              % (host, port, max_pkt, stop.decode("ascii", "replace")))
        region, c0, c1 = current_region(conn, max_pkt)
        voices, of, sd = snapshot(conn, region, max_pkt, source_config_word)
        detach(conn)
    print_report(voices, of, sd, region, c0, c1, label, show_all)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", required=True,
                    help="console IP (the 3DS running the Rosalina GDB stub)")
    ap.add_argument("--port", type=int, default=4000,
                    help="GDB stub TCP port. READ IT FROM THE ROSALINA "
                         "'Debugger options' screen — it shows the port per "
                         "selected process (4000..4002, sometimes 4003). "
                         "Default 4000.")
    ap.add_argument("--label", default=None,
                    help="tag this snapshot with the span/front_bypass value "
                         "you believe is active (from the MANIFEST time map)")
    ap.add_argument("--all-voices", action="store_true",
                    help="print all 24 voice slots, not just the active ones")
    ap.add_argument("--source-config-word", type=lambda s: int(s, 0),
                    default=WORD_SOURCE_CONFIG,
                    help="override SourceConfiguration region-0 word address "
                         "for a different firmware (default MiiPlaza 0x9E92)")
    ap.add_argument("--no-ack", action="store_true",
                    help="disable RSP ack framing (only if the stub negotiated "
                         "no-ack mode; Luma defaults to acks ON)")
    ap.add_argument("--timeout", type=float, default=10.0,
                    help="socket timeout seconds (default 10)")
    args = ap.parse_args()
    try:
        run(args.host, args.port, args.label, args.all_voices,
            args.source_config_word, not args.no_ack, args.timeout)
    except (OSError, IOError) as e:
        print("ERROR: %s" % e, file=sys.stderr)
        print("  - is the Rosalina debugger enabled AND MiiPlaza selected in "
              "the process list?", file=sys.stderr)
        print("  - is --port the exact port the Rosalina menu shows?",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
