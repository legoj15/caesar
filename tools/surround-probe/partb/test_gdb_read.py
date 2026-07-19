#!/usr/bin/env python3
"""Offline unit test for gdb_read.py — verifies the RSP framing, checksum, `}`
escape / `*` run-length decode, the chunked `m<addr>,<len>` memory read + hex
decode, the double-bank (higher-counter) selection, that the slot geometry is
PARSED FROM the single authority header, and the DUAL-DUMP decode (both the
candidate-A f32 gain[3][4] @ +0x04 AND the candidate-B s16 lanes @ +0x3c, plus
the raw slot head) WITHOUT a console. A fake Luma-style RSP stub runs on a
loopback socket in a background thread and serves a synthetic DSP-RAM image.

Run:  python test_gdb_read.py     (exit 0 = all pass)
  or: pytest tools/surround-probe/partb -q   (checks assert, so pytest fails red)
"""

import socket
import struct
import sys
import threading

import gdb_read as g

# --------------------------------------------------------------------------
# Build a synthetic DSP-RAM image (sparse dict of absolute VA -> byte). Only
# the addresses the client reads need to be populated.
# --------------------------------------------------------------------------

MEM = {}


def poke(addr, data: bytes):
    for i, b in enumerate(data):
        MEM[addr + i] = b


def peek(addr, length) -> bytes:
    return bytes(MEM.get(addr + i, 0) for i in range(length))


def make_source_config(gains12, enable, drv=None):
    """A 192-byte SourceConfiguration slot: candidate-A f32 gains @ +0x04, and
    (optionally) candidate-B u16 lanes @ +0x3c, plus enable."""
    slot = bytearray(g.SRC_CFG_STRIDE)
    struct.pack_into("<%df" % g.GAIN_HLE_COUNT, slot, g.GAIN_HLE_OFF, *gains12)
    if drv is not None:
        struct.pack_into("<%dH" % len(drv), slot, g.GAIN_DRV_OFF, *drv)
    slot[g.ENABLE_OFF] = enable
    return bytes(slot)


# Region 0 counter = 100, region 1 counter = 105 -> region 1 is CURRENT.
COUNTER0, COUNTER1 = 100, 105
poke(g.word_vaddr(g.WORD_FRAME_COUNTER, 0), struct.pack("<H", COUNTER0))
poke(g.word_vaddr(g.WORD_FRAME_COUNTER, 1), struct.pack("<H", COUNTER1))

# Voice 0 = the (rear-heavy) stimulus voice; voice 3 = an idle-ish front voice.
# These live in the CURRENT bank (region 1). The STALE bank (region 0) carries
# decoy values, so a correct reader that picks region 1 returns VOICE0_GAINS.
VOICE0_GAINS = (0.10, 0.10, 0.70, 0.70, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0)
VOICE3_GAINS = (0.90, 0.90, 0.00, 0.00, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0)
DECOY_GAINS = (0.5,) * 12
# Voice 0 also carries candidate-B s16 lanes @ +0x3c: FL+FR = 32768 (the
# constant-sum crossfade signature), rear-heavy RL/RR. The dual dump must decode
# these independently of the f32 @ +0x04.
VOICE0_DRV = (5243, 27525, 22937, 22937) + (0,) * (g.GAIN_DRV_COUNT - 4)

src_base_cur = g.word_vaddr(g.WORD_SOURCE_CONFIG, 1)
src_base_stale = g.word_vaddr(g.WORD_SOURCE_CONFIG, 0)
for v in range(g.NUM_SOURCES):
    poke(src_base_stale + v * g.SRC_CFG_STRIDE, make_source_config(DECOY_GAINS, 1))
    if v == 0:
        poke(src_base_cur + v * g.SRC_CFG_STRIDE,
             make_source_config(VOICE0_GAINS, 1, drv=VOICE0_DRV))
    elif v == 3:
        poke(src_base_cur + v * g.SRC_CFG_STRIDE, make_source_config(VOICE3_GAINS, 1))
    else:
        poke(src_base_cur + v * g.SRC_CFG_STRIDE, make_source_config((0.0,) * 12, 0))

# DspConfiguration in the current bank: output_format = 2 (Surround), depth 0.75
poke(g.word_vaddr(g.WORD_DSP_CONFIG, 1) + g.DSP_CFG_OUTPUT_FORMAT_OFF,
     struct.pack("<H", 2))
poke(g.word_vaddr(g.WORD_DSP_CONFIG, 1) + g.DSP_CFG_SURROUND_DEPTH_OFF,
     struct.pack("<f", 0.75))


# --------------------------------------------------------------------------
# Fake RSP stub server (one connection, ack framing on).
# --------------------------------------------------------------------------

class FakeStub(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(1)
        self.port = self.srv.getsockname()[1]
        self.bad_checksums = 0     # count client packets with a wrong checksum

    def run(self):
        conn, _ = self.srv.accept()
        buf = bytearray()
        with conn:
            while True:
                try:
                    chunk = conn.recv(4096)
                except OSError:
                    return
                if not chunk:
                    return
                buf += chunk
                while b"#" in buf:
                    start = buf.find(b"$")
                    end = buf.find(b"#", start)
                    if start < 0 or end < 0 or end + 2 >= len(buf):
                        break
                    body = bytes(buf[start + 1:end])
                    csum = bytes(buf[end + 1:end + 3])
                    del buf[:end + 3]
                    if int(csum, 16) != g.rsp_checksum(body):
                        self.bad_checksums += 1
                        conn.sendall(b"-")
                        continue
                    conn.sendall(b"+")
                    conn.sendall(g.build_packet(self._reply(body)))

    def _reply(self, body: bytes) -> bytes:
        if body.startswith(b"qSupported"):
            return b"PacketSize=4000;swbreak+;hwbreak+"
        if body == b"?":
            return b"S05"
        if body == b"D":
            return b"OK"
        if body.startswith(b"m"):
            addr_s, len_s = body[1:].split(b",")
            addr, length = int(addr_s, 16), int(len_s, 16)
            return peek(addr, length).hex().encode("ascii")
        return b""          # unsupported -> empty (RSP convention)


# --------------------------------------------------------------------------
# Tests
# --------------------------------------------------------------------------

def check(name, cond):
    # assert (not a counter) so the checks fail RED under pytest, which collects
    # each test_* function; standalone main() catches the AssertionError.
    print(("PASS" if cond else "FAIL") + "  " + name)
    assert cond, name


def test_authority_geometry():
    # The slot geometry must come from the ONE shared authority header, not be
    # hardcoded in the reader — this pins the parsed values.
    check("stride parsed == 192", g.SRC_CFG_STRIDE == 192)
    check("num sources parsed == 24", g.NUM_SOURCES == 24)
    check("enable off parsed == 0xA0", g.ENABLE_OFF == 0xA0)
    check("candidate A gain off parsed == 0x04", g.GAIN_HLE_OFF == 0x04)
    check("candidate A gain count parsed == 12", g.GAIN_HLE_COUNT == 12)
    check("candidate B gain off parsed == 0x3c", g.GAIN_DRV_OFF == 0x3C)
    check("raw dump window parsed == 0x50 bytes", g.SLOT_DUMP_BYTES == 0x50)


def test_framing():
    # Known vector: checksum of "OK" = ('O'+'K') & 0xff = 0x4f+0x4b = 0x9a.
    check("checksum('OK') == 0x9a", g.rsp_checksum(b"OK") == 0x9A)
    check("build_packet('OK') == $OK#9a", g.build_packet(b"OK") == b"$OK#9a")
    # m-packet builds correctly (lowercase hex, no 0x).
    check("build m-packet", g.build_packet(b"m1ff53d24,30")
          == b"$m1ff53d24,30#" + ("%02x" % g.rsp_checksum(b"m1ff53d24,30")).encode())


def test_rle_escape():
    # '*' run-length: '0' then '* '(0x20 -> repeat 32-29=3 extra) => '0000'.
    check("RLE decode 0*<sp> -> 0000",
          g.rsp_decode_body(b"0" + bytes([0x2A, 0x20])) == b"0000")
    # '}' escape: 0x7d 0x03 -> 0x03 ^ 0x20 = 0x23 ('#').
    check("brace-escape decode", g.rsp_decode_body(bytes([0x7D, 0x03])) == b"#")
    check("plain body unchanged", g.rsp_decode_body(b"deadbeef") == b"deadbeef")


def test_addressing():
    # region-1 word = region-0 word | 0x10000; VA = 0x1FF40000 + word*2.
    check("src cfg region0 VA", g.word_vaddr(g.WORD_SOURCE_CONFIG, 0)
          == 0x1FF40000 + 0x9E92 * 2)
    check("src cfg region1 VA", g.word_vaddr(g.WORD_SOURCE_CONFIG, 1)
          == 0x1FF40000 + (0x9E92 | 0x10000) * 2)
    check("counter_newer wrap (105>100)", g.counter_newer(105, 100))
    check("counter_newer wrap (0x0001>0xFFFF)", g.counter_newer(0x0001, 0xFFFF))
    check("counter_newer false (100 !> 105)", not g.counter_newer(100, 105))


def test_live_against_fake():
    stub = FakeStub()
    stub.start()
    with socket.create_connection(("127.0.0.1", stub.port), timeout=5) as sock:
        conn = g.RspConnection(sock, use_ack=True)
        max_pkt, stop = g.handshake(conn)
        check("handshake PacketSize=0x4000", max_pkt == 0x4000)
        check("handshake stop reason S05", stop == b"S05")

        # raw m read + hex decode of the region-1 frame counter.
        raw = g.read_mem(conn, g.word_vaddr(g.WORD_FRAME_COUNTER, 1), 2, max_pkt)
        check("read_mem hex-decodes u16 counter",
              struct.unpack("<H", raw)[0] == COUNTER1)

        region, c0, c1 = g.current_region(conn, max_pkt)
        check("current_region picks the higher-counter bank (1)", region == 1)
        check("counters read back (100,105)", (c0, c1) == (COUNTER0, COUNTER1))

        voices, of, sd = g.snapshot(conn, region, max_pkt)
        g.detach(conn)

    v0 = voices[0]
    check("voice0 enable==1", v0[1] == 1)
    check("voice0 gain[3][4] decodes to VOICE0_GAINS",
          all(abs(a - b) < 1e-6 for a, b in zip(v0[2], VOICE0_GAINS)))
    check("voice0 MAIN rear lanes are the high ones (0.70)",
          abs(v0[2][2] - 0.70) < 1e-6 and abs(v0[2][3] - 0.70) < 1e-6)
    check("voice3 is front-heavy (rear ~0)",
          abs(voices[3][2][2]) < 1e-6 and abs(voices[3][2][0] - 0.90) < 1e-6)
    active = [v for v in voices if v[1] != 0]
    check("exactly 2 active voices (0 and 3)",
          [v[0] for v in active] == [0, 3])
    check("output_format == 2 (Surround)", of == 2)
    check("surround_depth == 0.75", abs(sd - 0.75) < 1e-6)
    check("no client packet had a bad checksum", stub.bad_checksums == 0)

    # --- DUAL DUMP: candidate B (s16 @ +0x3c) + raw head, independent of A ---
    check("voice0 candidate B s16 lanes decode to VOICE0_DRV",
          tuple(v0[3]) == VOICE0_DRV)
    check("voice0 candidate B FL+FR == 32768 (crossfade signature)",
          v0[3][0] + v0[3][1] == 32768)
    check("voice0 raw slot head is SLOT_DUMP_BYTES long",
          len(v0[4]) == g.SLOT_DUMP_BYTES)
    check("voice0 raw head preserves the f32 @ +0x04 (candidate A byte 0)",
          abs(struct.unpack_from("<f", v0[4], g.GAIN_HLE_OFF)[0]
              - VOICE0_GAINS[0]) < 1e-6)
    check("voice0 raw head preserves the u16 @ +0x3c (candidate B lane 0)",
          struct.unpack_from("<H", v0[4], g.GAIN_DRV_OFF)[0] == VOICE0_DRV[0])

    # Render the report once to prove the dual-dump report doesn't throw.
    g.print_report(voices, of, sd, region, c0, c1, label="span=127",
                   show_all=False)


def main():
    try:
        test_authority_geometry()
        test_framing()
        test_rle_escape()
        test_addressing()
        test_live_against_fake()
    except AssertionError as e:
        print("-" * 40)
        print("CHECK FAILED: %s" % e)
        return 1
    print("-" * 40)
    print("ALL CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
