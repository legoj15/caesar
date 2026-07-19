#!/usr/bin/env python3
"""Surround Part B — one-shot GDB-RSP reader that DUAL-DUMPS both candidate
per-voice gain layouts so ONE console session settles which one is live.

Binds the NW4C `span` (0xD7) command to its cause. While the Part B stimulus
cartridge (`build_partb_cartridge.py`) holds a note and sweeps `span`, this
client attaches to StreetPass Mii Plaza over the console's **Luma3DS Rosalina
GDB stub** (RSP over TCP), reads the live per-voice SourceConfiguration out of
the DSP shared region mapped in the game's address space, and prints — for each
active voice — BOTH candidate gain interpretations plus the raw slot bytes. A
one-shot ALL-STOP snapshot: attaching halts the ARM11 process, so the config is
frozen at its last-written state (which reflects the span value active at that
instant), we read, then detach and the game resumes.

The DSP shared-memory addressing is IDENTICAL to the in-process dsp-tap plugin
(tools/dsp-tap): the GDB `m<addr>,<len>` packet reads the *debuggee's* virtual
memory, and the DSP RAM is mapped at the same fixed VA (0x1FF00000, data at
0x1FF40000) in MiiPlaza's process — so the same address arithmetic applies.

=============================================================================
THE OPEN QUESTION THIS TOOL SETTLES
=============================================================================
Two rival layouts for the front/rear pan gains inside the 192-byte voice slot:
  * Candidate A — Azahar HLE:  float_le gain[3][4] @ slot+0x04 (12 f32, 48 B)
  * Candidate B — real ARM11 driver (disasm NW4C-handoff Session 7):
                  s16 gains @ slot+0x3c (FL@+3c, FR@+3e, RL@+40, RR@+42, ...)
SETTLED 2026-07-19 by the live run below — Candidate A (f32 @ +0x04) WINS: on the
stimulus voice the +0x04 floats tracked span through a constant-power front<->rear
crossfade while the s16@+0x3c lanes were a fixed constant in every voice (see the
verdict in the shared authority header `tools/dsp-shared/dsp_gain_geometry.h`,
now DSP_SRC_GAIN_SETTLED=1). This reader is RETAINED as a re-verification /
future-firmware tool and still dual-dumps both candidates + the raw slot head, so
the record stays self-describing. The geometry constants below are PARSED FROM
that one authority — not duplicated.

=============================================================================
LIVE RUN — exact command line
=============================================================================
  1) System Settings -> Sound -> Surround   (rear lanes only lift in Surround)
  2) Deploy the built MeetSound.bcsar (see README Sec.1) + enable Game patching
  3) Rosalina (L+Down+Select) -> Debugger options -> Enable debugger; attach
     MiiPlaza in the Process list; NOTE THE PORT the menu shows (4000..4003)
  4) Plaza -> Music Player -> the hijacked track; start a stopwatch at pilot tone
  5) For each span window in build/partb/MANIFEST.md, run:

     python gdb_read.py --host <console-ip> --port <port> --label span=64
        # --all-voices to dump all 24 slots

THE VERDICT (2026-07-19 live run, stimulus voice = the sustained one whose
balance shifts with span):
  * candidate-A floats WERE gains (MAIN front fell & rear rose with span: 0.7071/0
    -> 0.6108/0.3563 -> 0.3452/0.6171, front^2+rear^2 = 0.500 throughout) AND
    candidate-B s16 lanes were a fixed constant (RL=59910,RR=11793) in every voice
        => +0x04 (Candidate A) IS the live shared-memory layout.  [CONFIRMED]
  * +0x3c (Candidate B) is DISPROVEN as the shared-mem field — it is the ARM11-side
    intermediate the disasm found, which the DSP-region read sees as stale bytes.
The follow-up edit is DONE: `tools/dsp-shared/dsp_gain_geometry.h` carries
DSP_SRC_GAIN_SETTLED=1, canonical = Candidate A, DSP_SRC_GAIN_DRV_DISPROVEN=1.
Re-run this reader only to re-verify or to characterize a DIFFERENT firmware.
=============================================================================

The RSP framing + hex decode + the dual-dump decode are unit-tested offline
against a fake stub (`test_gdb_read.py`) — no console needed for the packet or
decode layers.

Stdlib only. Read-only: the client never writes console memory.
"""

import argparse
import os
import re
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

# ---------------------------------------------------------------------------
# SourceConfiguration slot geometry — PARSED FROM THE ONE AUTHORITY so this
# reader, the oracle (shared_mem.h) and the dsp-tap plugin (dsp_regions.h)
# cannot re-diverge. Both candidate gain layouts live there with the verdict.
# ---------------------------------------------------------------------------
_AUTHORITY_HEADER = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "..", "dsp-shared", "dsp_gain_geometry.h"))


def parse_geometry(path):
    """Extract `#define NAME <int>` pairs (hex or decimal) from the shared C
    authority header, so the Python reader literally consumes the same source of
    truth the C/C++ tools include. Trailing comments are ignored."""
    defs = {}
    pat = re.compile(r"^\s*#define\s+(\w+)\s+(0[xX][0-9A-Fa-f]+|\d+)\b")
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            m = pat.match(line)
            if m:
                defs[m.group(1)] = int(m.group(2), 0)
    missing = [k for k in (
        "DSP_SRC_CFG_STRIDE", "DSP_NUM_SOURCES", "DSP_SRC_ENABLE_OFF",
        "DSP_SRC_GAIN_HLE_OFF", "DSP_SRC_GAIN_HLE_COUNT",
        "DSP_SRC_GAIN_DRV_OFF", "DSP_SRC_GAIN_DRV_COUNT",
        "DSP_SRC_SLOT_DUMP_OFF", "DSP_SRC_SLOT_DUMP_BYTES") if k not in defs]
    if missing:
        raise KeyError("authority header %s missing: %s" % (path, missing))
    return defs


_GEO = parse_geometry(_AUTHORITY_HEADER)

SRC_CFG_STRIDE = _GEO["DSP_SRC_CFG_STRIDE"]   # 192 B/source
NUM_SOURCES = _GEO["DSP_NUM_SOURCES"]         # 24
ENABLE_OFF = _GEO["DSP_SRC_ENABLE_OFF"]       # u8 (1 = voice active)

# Candidate A — Azahar HLE: float_le gain[3][4] @ slot+0x04.
GAIN_HLE_OFF = _GEO["DSP_SRC_GAIN_HLE_OFF"]
GAIN_HLE_COUNT = _GEO["DSP_SRC_GAIN_HLE_COUNT"]      # 12
GAIN_HLE_BYTES = GAIN_HLE_COUNT * 4                 # 48

# Candidate B — real ARM11 driver (disasm Session 7): s16 gains @ slot+0x3c.
GAIN_DRV_OFF = _GEO["DSP_SRC_GAIN_DRV_OFF"]
GAIN_DRV_COUNT = _GEO["DSP_SRC_GAIN_DRV_COUNT"]      # 8 lanes (FL,FR,RL,RR + aux)
GAIN_DRV_BYTES = GAIN_DRV_COUNT * 2                  # 16

# Raw slot head — dumped verbatim so a wrong assumption loses nothing.
SLOT_DUMP_OFF = _GEO["DSP_SRC_SLOT_DUMP_OFF"]        # 0x00
SLOT_DUMP_BYTES = _GEO["DSP_SRC_SLOT_DUMP_BYTES"]    # 0x50

# DspConfiguration fields used as an in-band cross-check (surround only lifts
# the rear lanes when the console is actually in Surround output mode):
DSP_CFG_OUTPUT_FORMAT_OFF = 0x16   # u16 OutputFormat
DSP_CFG_SURROUND_DEPTH_OFF = 0x1C  # float_le
OUTPUT_FORMAT_NAMES = {0: "Mono", 1: "Stereo", 2: "Surround"}

# gain[mixer][channel] labels. mixer 0 = MAIN (feeds the DAC), 1/2 = aux/effect
# buses. channel 0/1 = front L/R, 2/3 = rear L/R (the span axis lives here).
MIXER_NAMES = ("MAIN", "AUX-A(reverb)", "AUX-B(chorus)")
CHANNEL_NAMES = ("frontL", "frontR", "rearL ", "rearR ")

# Candidate-B s16 lane labels (disasm Session 7: FL@+3c, FR@+3e, RL@+40, RR@+42,
# then aux/extra lanes). Indices past the four named ones are shown as x@+off.
DRV_LANE_NAMES = ("FL@+3c", "FR@+3e", "RL@+40", "RR@+42")


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
    """Split a SourceConfiguration array blob into per-voice records carrying
    BOTH candidate gain interpretations plus the raw slot head:
        (index, enable, gains_hle[12] f32, gains_drv[N] u16, raw_head bytes)
    gains_hle = Candidate A (float_le @ +0x04); gains_drv = Candidate B (u16 @
    +0x3c). The live read compares the two against span to settle which is live;
    raw_head preserves the bytes so nothing rides on a wrong assumption."""
    voices = []
    for v in range(NUM_SOURCES):
        slot = raw[v * SRC_CFG_STRIDE:(v + 1) * SRC_CFG_STRIDE]
        enable = slot[ENABLE_OFF]
        gains_hle = struct.unpack("<%df" % GAIN_HLE_COUNT,
                                  slot[GAIN_HLE_OFF:GAIN_HLE_OFF + GAIN_HLE_BYTES])
        # u16 (unsigned): gains are magnitudes; a constant-sum crossfade
        # (FL+FR ~= 32768) is the Session-7 signature of the real pan law.
        gains_drv = struct.unpack("<%dH" % GAIN_DRV_COUNT,
                                  slot[GAIN_DRV_OFF:GAIN_DRV_OFF + GAIN_DRV_BYTES])
        raw_head = bytes(slot[SLOT_DUMP_OFF:SLOT_DUMP_OFF + SLOT_DUMP_BYTES])
        voices.append((v, enable, gains_hle, gains_drv, raw_head))
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

def format_gain_hle(gains):
    """Candidate A — float gain[3][4] laid out as 3 mixers x 4 channels."""
    lines = []
    for m in range(3):
        row = gains[m * 4:m * 4 + 4]
        cells = "  ".join("%s=%+.4f" % (CHANNEL_NAMES[c], row[c]) for c in range(4))
        lines.append("          [%-13s] %s" % (MIXER_NAMES[m], cells))
    return "\n".join(lines)


def format_gain_drv(gains):
    """Candidate B — s16/u16 lanes @ +0x3c, with the FL+FR constant-sum check."""
    parts = []
    for i, val in enumerate(gains):
        name = DRV_LANE_NAMES[i] if i < len(DRV_LANE_NAMES) \
            else "x@+%02x" % (GAIN_DRV_OFF + i * 2)
        parts.append("%s=%6d" % (name, val))
    line = "          " + "  ".join(parts)
    if len(gains) >= 2:
        s = gains[0] + gains[1]
        line += ("\n          (FL+FR=%d %s 32768 -> %s)"
                 % (s, "==" if s == 32768 else "!=",
                    "constant-sum crossfade signature"
                    if s == 32768 else "NOT the pan-law signature"))
    return line


def format_raw_head(raw):
    """Verbatim hex of the slot head (+0x00..) — the ground truth if both
    candidate decodes disagree with expectation."""
    lines = []
    for off in range(0, len(raw), 16):
        chunk = raw[off:off + 16]
        hexs = " ".join("%02x" % b for b in chunk)
        lines.append("          +0x%02x: %s" % (SLOT_DUMP_OFF + off, hexs))
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
        print("  [cross-check] output mode is %s -> MAIN rear lanes should be "
              "~0 regardless of span" % of_name)
    print("  DUAL DUMP: gains are read BOTH ways per voice. SETTLED 2026-07-19: "
          "Candidate A (f32 @ +0x04) is the live layout; Candidate B (+0x3c) is a "
          "disproven ARM11 intermediate. Both kept for re-verification.")
    print("    candidate A = f32 gain[3][4] @ slot+0x%02x  (Azahar HLE) [LIVE]"
          % GAIN_HLE_OFF)
    print("    candidate B = s16 lanes      @ slot+0x%02x  (disasm S7) [disproven]"
          % GAIN_DRV_OFF)
    print("  On the stimulus voice, Candidate A's MAIN front/rear tracks span "
          "(the confirmed layout; see the file header for the full verdict).")
    print("-" * 72)

    active = [v for v in voices if v[1] != 0]
    shown = voices if show_all else (active if active else voices[:1])
    if not active:
        print("WARNING: no voice has enable!=0 — is a note actually sounding? "
              "(showing voice 0)")
    for v, enable, gains_hle, gains_drv, raw_head in shown:
        tag = "ACTIVE" if enable else "idle"
        note = ""
        if enable:
            note = "  <-- A.MAIN rear=(%+.4f,%+.4f)  B.RL/RR=(%d,%d)" % (
                gains_hle[2], gains_hle[3],
                gains_drv[2] if len(gains_drv) > 2 else 0,
                gains_drv[3] if len(gains_drv) > 3 else 0)
        print("voice %2d  enable=%d (%s)%s" % (v, enable, tag, note))
        print("      candidate A (Azahar HLE   f32 @ +0x%02x):" % GAIN_HLE_OFF)
        print(format_gain_hle(gains_hle))
        print("      candidate B (disasm S7    s16 @ +0x%02x):" % GAIN_DRV_OFF)
        print(format_gain_drv(gains_drv))
        print("      raw slot +0x%02x..+0x%02x:"
              % (SLOT_DUMP_OFF, SLOT_DUMP_OFF + SLOT_DUMP_BYTES))
        print(format_raw_head(raw_head))
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
