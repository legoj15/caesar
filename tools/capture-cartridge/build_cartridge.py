#!/usr/bin/env python3
"""Build the console capture cartridge: a surgically patched MeetSound.bcsar.

The patched archive turns TWO music-player tracks of StreetPass Mii Plaza
into hands-free calibration batteries played by the console's own NW4C
engine (via Luma3DS LayeredFS whole-file override):

  - track A, BGM_MAIN_Mii_Only_One  -> the SE_MAIN-bank battery
    (drum takes, pan probes, slide, velocity ladder)
  - track B, BGM_DEN_EMPTY_LANDSCAPE -> the LEGEND-bank battery
    (key-fly release witness, cursor attack, portamento glide)

Each hijacked entry gets its INFO volume byte set to 127 (a known
reference level) and its INFO bank index re-pointed to the bank its
sections need; the embedded .bcseq DATA payloads are rewritten in place
(same size, zero-padded) with probes assembled from the target sound
effects' own command bytes, lifted verbatim from the same archive.

The battery deliberately contains NO 0xB6 bank-switch command: the first
console test (2026-07-14) came back silent with 0xB6 in the stream, and
whether hardware reads its argument as a global bank index (caesar's
interpretation) or a per-sound bank-slot index is unresolved. Each track
keeps one bank, set through the INFO entry — the exact mechanism retail
BGM playback uses. The two-bank split doubles as a differential: both
tracks silent => bank loading is the problem, not command semantics.

Everything is located BY NAME through the archive's own STRG/INFO tables,
so the tool keeps working if entry order differs in another archive
version. All patches are same-size and in place: no offset in the file
moves, no length word changes, and the stage-1 serializer round-trips the
result byte-identically.

Stdlib only. Never writes to the source path; output goes to --out.
"""

import argparse
import hashlib
import sys
from pathlib import Path

# ---------------------------------------------------------------- constants

TICKS_PER_SEC = 96.0  # tempo 120 BPM x timebase 48 / 60 (both explicit/default)

SEQ_HOST_A = "BGM_MAIN_Mii_Only_One"       # music player "Main Theme 1"
SEQ_HOST_B = "BGM_DEN_EMPTY_LANDSCAPE"     # the 2026-07-08 captured den track
SEQ_DRUM = "SE_NEW_DRUM01"
SEQ_KEYFLY = "SE_LEGEND_KEY_FLY"
SEQ_CURSOR = "SE_LEGEND_MENU_CURSOR"
SEQ_SLIDE = "SE_NEW_SLIDE_MAP"

DEFAULT_TID = "0004001000021800"  # StreetPass Mii Plaza (USA), base title
# The plaza's v14 update romfs moved the archive (the base title's
# sound/MeetSound.bcsar path is never requested by the updated game).
DEFAULT_ROMFS_REL = "romfs/region_common/frame/sound/MeetSound.bcsar"

# ------------------------------------------------------------- byte helpers


def u16(b, o):
    return b[o] | (b[o + 1] << 8)


def u32(b, o):
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)


def enc_varint(v):
    """SMF-style VLQ: big-endian 7-bit groups, high bit = continue."""
    if v < 0:
        raise ValueError("varint must be non-negative")
    groups = [v & 0x7F]
    v >>= 7
    while v:
        groups.append((v & 0x7F) | 0x80)
        v >>= 7
    return bytes(reversed(groups))


def read_varint(b, p):
    v = 0
    while True:
        byte = b[p]
        p += 1
        v = (v << 7) | (byte & 0x7F)
        if not (byte & 0x80):
            return v, p


# ---------------------------------------------------------- BCSAR mini-parse


class Bcsar:
    """Just enough BCSAR to locate sound entries and embedded .bcseq files.

    Offset recipes match src/Csar.cpp exactly (INFO record head, the
    CbnkOffset-anchored sub-struct, the FILE 0x220C indirection).
    """

    def __init__(self, data: bytes):
        self.data = data
        if data[0:4] != b"CSAR" or u16(data, 4) != 0xFEFF:
            raise ValueError("not a little-endian BCSAR")
        self.strg_off = u32(data, 0x18)
        self.info_off = u32(data, 0x24)
        self.file_off = u32(data, 0x30)

        # STRG symbol table
        strg = self.strg_off
        if data[strg:strg + 4] != b"STRG":
            raise ValueError("STRG magic mismatch")
        count = u32(data, strg + 0x18)
        self.strings = []
        for i in range(count):
            e = strg + 0x1C + i * 12
            if u32(data, e) != 0x1F01:
                raise ValueError(f"STRG entry {i} magic mismatch")
            rel, ln = u32(data, e + 4), u32(data, e + 8)
            s = strg + 0x18 + rel
            self.strings.append(data[s:s + ln - 1].decode("ascii"))

        # INFO category reference block
        base = self.info_off + 8
        if data[self.info_off:self.info_off + 4] != b"INFO":
            raise ValueError("INFO magic mismatch")
        cats = {}
        for i in range(8):
            cid = u32(data, base + i * 8)
            cats[cid] = base + u32(data, base + i * 8 + 4)
        self.cseq_table = cats[0x2100]
        self.cbnk_table = cats[0x2101]
        self.file_table = cats[0x2106]

    def _category_records(self, table, magic):
        count = u32(self.data, table)
        out = []
        for i in range(count):
            e = table + 4 + i * 8
            if u32(self.data, e) != magic:
                raise ValueError(f"category entry {i} magic != {magic:#x}")
            out.append(table + u32(self.data, e + 4))
        return out

    def sound_entries(self):
        """(index, record_abs, name, type) for every 0x2100 sound entry."""
        for i, rec in enumerate(self._category_records(self.cseq_table, 0x2200)):
            name = self.strings[u32(self.data, rec + 0x18)]
            yield i, rec, name, u32(self.data, rec + 0x0C)

    def sound_by_name(self, name):
        for i, rec, n, typ in self.sound_entries():
            if n == name:
                if typ != 0x2203:
                    raise ValueError(f"{name} is not an internal sequence entry")
                return SoundEntry(self, i, rec)
        raise ValueError(f"sound entry not found: {name}")

    def bank_name(self, index):
        recs = self._category_records(self.cbnk_table, 0x2206)
        return self.strings[u32(self.data, recs[index] + 0x10)]

    def file_span(self, file_index):
        """(abs_start, length) of an internal 0x220C embedded file."""
        recs = self._category_records(self.file_table, 0x220A)
        if file_index >= len(recs):
            raise ValueError(f"file index {file_index} out of range")
        frec = recs[file_index]
        if u32(self.data, frec) != 0x220C:
            raise ValueError(f"file {file_index} is not an internal 0x220C record")
        start = self.file_off + 8 + u32(self.data, frec + 0x10)
        length = u32(self.data, frec + 0x14)
        if length == 0xFFFFFFFF or start + length > len(self.data):
            raise ValueError(f"file {file_index} is external/absent")
        return start, length


class SoundEntry:
    def __init__(self, ar: Bcsar, index, rec):
        d = ar.data
        self.archive = ar
        self.index = index
        self.rec = rec
        self.file_index = u32(d, rec + 0x00)
        self.volume_off = rec + 0x08          # low byte of Word08
        self.volume = d[self.volume_off]
        c = u32(d, rec + 0x10)                # CbnkOffset sub-struct anchor
        self.start_off_field = rec + c + 0x10
        self.start_offset = u32(d, self.start_off_field)
        self.bank_field = rec + 0x1C + c      # u16
        self.bank_index = u16(d, self.bank_field)

    def cseq_stream(self):
        """(stream_abs, stream_len) = the embedded .bcseq DATA payload
        (past the 8-byte DATA header) — the safe overwrite window."""
        d = self.archive.data
        start, length = self.archive.file_span(self.file_index)
        if d[start:start + 4] != b"CSEQ":
            raise ValueError("embedded file is not a CSEQ")
        if u32(d, start + 0x0C) != length:
            raise ValueError("CSEQ self-length disagrees with FILE record")
        data_off = u32(d, start + 0x18)
        data_len = u32(d, start + 0x1C)
        if d[start + data_off:start + data_off + 4] != b"DATA":
            raise ValueError("CSEQ DATA magic mismatch")
        return start + data_off + 8, data_len - 8


# ------------------------------------------------------------ command walker


class Run:
    """A lifted command run: verbatim bytes, tick advance, note positions."""

    def __init__(self, data, ticks, notes, notewait_end, touched):
        self.data = data          # bytes, Fin excluded
        self.ticks = ticks
        self.notes = notes        # [(vel_byte_rel_off, key, vel, len)]
        self.notewait_end = notewait_end
        self.touched = touched    # subset of {"volume", "pan", "notewait"}
        self.bank = None          # global CbnkRecords index, set from INFO


def walk_run(stream: bytes, start: int, name: str) -> Run:
    """Walk one entry's commands from its start offset to its Fin, applying
    src/Cseq.cpp's exact argument table. Deterministic-replica rules: any
    command that could change control flow or randomize (jump/call/open-
    track/Rnd/Var/If/extended-VM/counted-loop) is refused, so the lifted
    bytes replay identically wherever they are placed."""
    p = start
    ticks = 0
    notewait = True
    notes = []
    touched = set()
    while True:
        b = stream[p]
        p += 1
        if b in (0xA0, 0xA1, 0xA2, 0xA4, 0xA5):
            raise ValueError(f"{name}: non-deterministic prefix {b:#x} in replica")
        suffix_t = False
        if b == 0xA3:
            suffix_t = True
            b = stream[p]
            p += 1
            if b in (0xA0, 0xA1):
                raise ValueError(f"{name}: Rnd/Var prefix {b:#x} in replica")

        if b < 0x80:                                   # note: key vel varlen
            vel_off = p - start
            vel = stream[p]
            p += 1
            ln, p = read_varint(stream, p)
            notes.append((vel_off, b, vel, ln))
            if notewait:
                ticks += ln
        elif b == 0x80:
            w, p = read_varint(stream, p)
            ticks += w
        elif b == 0x81:
            _, p = read_varint(stream, p)
        elif b in (0x88, 0x89, 0x8A, 0xFC, 0xFD, 0xFE):
            raise ValueError(f"{name}: control-flow opcode {b:#x} in replica")
        elif b == 0xFB:
            pass
        elif b == 0xFF:
            return Run(bytes(stream[start:p - 1]), ticks, notes, notewait, touched)
        elif 0xB0 <= b <= 0xDF:
            if 0xB7 <= b <= 0xBC or b in (0xB6, 0xD4):
                raise ValueError(f"{name}: bank-switch/loop opcode {b:#x} in replica")
            arg = stream[p]
            p += 1
            if b == 0xC7:
                notewait = arg != 0
                touched.add("notewait")
            elif b in (0xC1, 0xC2, 0xD5):
                touched.add("volume")
            elif b in (0xC0, 0xDC):
                touched.add("pan")
        elif b in (0xE0, 0xE1, 0xE3, 0xE4):
            p += 2
        elif b == 0xF0:
            raise ValueError(f"{name}: extended VM opcode in replica")
        else:
            raise ValueError(f"{name}: unknown opcode {b:#x} at run+{p - 1 - start:#x}")
        if suffix_t:
            p += 2                                     # trailing s16 ramp time


def check_payload(payload: bytes):
    """Re-parse the ENTIRE rewritten DATA payload the way src/Cseq.cpp will:
    linearly, command by command, from byte 0 (the head zeros parse as one
    silent phantom note) through the battery, its loop jump, and the zero
    pad. Must consume every byte exactly — no overshoot into the LABL block."""
    p = 0
    while p < len(payload):
        b = payload[p]
        p += 1
        suffix_t = False
        if b == 0xA2:
            b = payload[p]
            p += 1
        if b == 0xA3:
            suffix_t = True
            b = payload[p]
            p += 1
        if b in (0xA0, 0xA1):
            raise ValueError("Rnd/Var in battery payload")
        if b < 0x80:
            p += 1
            _, p = read_varint(payload, p)
        elif b in (0x80, 0x81):
            _, p = read_varint(payload, p)
        elif b == 0x88:
            p += 4
        elif b in (0x89, 0x8A):
            p += 3
        elif 0xB0 <= b <= 0xDF:
            if 0xB7 <= b <= 0xBC:
                raise ValueError(f"invalid opcode {b:#x} in payload")
            p += 1
        elif b in (0xE0, 0xE1, 0xE3, 0xE4):
            p += 2
        elif b in (0xFB, 0xFC, 0xFD, 0xFF):
            pass
        elif b == 0xFE:
            p += 2
        else:
            raise ValueError(f"unparseable opcode {b:#x} at payload+{p - 1:#x}")
        if suffix_t:
            p += 2
    if p != len(payload):
        raise ValueError(f"payload overshoot: parse ended at {p}, len {len(payload)}")


# --------------------------------------------------------- battery assembler


class Battery:
    """One track's probe stream. `bank` is fixed for the whole battery —
    set via the hijacked entry's INFO record, never via 0xB6 (see module
    docstring). require_bank() catches a replica placed on the wrong track."""

    def __init__(self, bank):
        self.buf = bytearray()
        self.tick = 0
        self.events = []  # (tick, section, label)
        self.bank = bank

    def require_bank(self, bank):
        if bank != self.bank:
            raise ValueError(f"replica needs bank {bank}, battery is {self.bank}")

    def mark(self, section, label):
        self.events.append((self.tick, section, label))

    def emit(self, data: bytes, adv=0):
        self.buf += data
        self.tick += adv

    def rest_seconds(self, seconds):
        ticks = round(seconds * TICKS_PER_SEC)
        self.emit(b"\x80" + enc_varint(ticks), adv=ticks)

    def note(self, key, vel, length):
        self.emit(bytes([key, vel]) + enc_varint(length), adv=length)

    def replica(self, run: Run, section, label, ring_seconds, vel=None):
        """Verbatim SE replay + ring-out. State restores (volume/pan) are
        deferred until AFTER the ring-out so a releasing tail keeps following
        the same track timeline it would in the retail sequence."""
        self.require_bank(run.bank)
        data = bytearray(run.data)
        if vel is not None:
            for off, _key, _v, _ln in run.notes:
                data[off] = vel
        self.mark(section, label)
        self.emit(bytes(data), adv=run.ticks)
        self.emit(b"\xfb")                    # reset latched ADSHR overrides
        if not run.notewait_end:
            self.emit(b"\xc7\x01")            # restore note-wait default
        self.rest_seconds(ring_seconds - run.ticks / TICKS_PER_SEC)
        if "volume" in run.touched:
            self.emit(b"\xc1\x7f\xc2\x7f\xd5\x7f")
        if "pan" in run.touched:
            self.emit(b"\xc0\x40\xdc\x40")

    def setup(self):
        # explicit tempo/volume/pan so every state is deterministic
        self.mark("setup", "tempo 120, volume 127, pan center")
        self.emit(b"\xe1\x00\x78\xc1\x7f\xc0\x40")
        self.rest_seconds(1.0)

    def finish(self, loop_target):
        self.mark("loop", "end of pass; hardware jumps back to start")
        self.rest_seconds(2.0)
        # backward jump: retail whole-song loop form — hardware repeats
        # forever, caesar-play renders one pass.
        self.emit(bytes([0x89]) + loop_target.to_bytes(3, "big"))


def assemble_a(drum: Run, slide: Run, loop_target):
    """Track A — everything in the drum/slide bank (BANK_MEET_SE_MAIN)."""
    bat = Battery(drum.bank)
    bat.setup()
    for i in range(3):                        # A1: decay-table / level takes
        bat.replica(drum, "A1", f"SE_NEW_DRUM01 hit {i + 1}/3 (vel 127)", 2.5)
    for pan, tag in ((0x00, "hard left"), (0x7F, "hard right"), (0x40, "center")):
        bat.emit(bytes([0xC0, pan]))
        bat.replica(drum, "A2", f"pan probe {tag} (DRUM01)", 2.0)
    bat.replica(slide, "A3", "SE_NEW_SLIDE_MAP (LPF probe)", 2.5)
    for vel in (96, 64, 32):                  # A4: velocity-squared law
        bat.replica(drum, "A4", f"DRUM01 velocity ladder (vel {vel})", 1.5,
                    vel=vel)
    bat.finish(loop_target)
    return bat


def assemble_b(keyfly: Run, cursor: Run, loop_target):
    """Track B — everything in the key-fly/cursor bank (BANK_MEET_LEGEND)."""
    bat = Battery(keyfly.bank)
    bat.setup()
    bat.replica(keyfly, "B1", "SE_LEGEND_KEY_FLY (release/LFO witness)", 12.0)
    bat.replica(cursor, "B2", "SE_LEGEND_MENU_CURSOR (attack probe)", 1.5)
    # B3: portamento glide on KEY_FLY's sustained instrument (program 23 in
    # its own bank): origin note, then porta on + time byte 48, glide to
    # key 74 over the next note. The glide DURATION mapping is the flagged
    # constant this section exists to measure.
    bat.mark("B3", "porta origin (key 50)")
    bat.emit(b"\x81\x17")
    bat.note(50, 127, 48)
    bat.emit(b"\xce\x01\xcf\x30")
    bat.mark("B3", "porta glide 50->74 (time byte 48)")
    bat.note(74, 127, 192)
    bat.emit(b"\xce\x00")
    bat.rest_seconds(4.0)
    bat.finish(loop_target)
    return bat


# ------------------------------------------------------------------- patcher


def contiguous_diff_ranges(a: bytes, b: bytes):
    ranges = []
    start = None
    for i in range(len(a)):
        if a[i] != b[i]:
            if start is None:
                start = i
        elif start is not None:
            ranges.append((start, i))
            start = None
    if start is not None:
        ranges.append((start, len(a)))
    return ranges


def patch_track(patched, host, bat):
    """Rewrite one host entry's DATA payload with its battery and poke its
    INFO volume + bank. Returns (stream_abs, stream_len)."""
    stream_abs, stream_len = host.cseq_stream()
    payload_off = host.start_offset  # keep the entry's start untouched
    # Keep the zero-pad tail a multiple of 3 so it parses as whole silent
    # phantom notes (00 00 00) right up to the DATA boundary — the parser
    # walks the entire payload and must never overshoot into LABL. The
    # 0xFB padding (ADSHR reset, no args) sits after the loop jump: never
    # executed, always parseable.
    tail = stream_len - payload_off - len(bat.buf)
    if tail < 0:
        raise ValueError(f"battery ({len(bat.buf)} B) exceeds the DATA window "
                         f"({stream_len} B)")
    bat.emit(b"\xfb" * (tail % 3))
    tail = stream_len - payload_off - len(bat.buf)

    patched[stream_abs:stream_abs + stream_len] = (
        b"\x00" * payload_off + bat.buf + b"\x00" * tail)
    patched[host.volume_off] = 127
    patched[host.bank_field] = bat.bank & 0xFF
    patched[host.bank_field + 1] = (bat.bank >> 8) & 0xFF
    return stream_abs, stream_len


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--source", required=True, help="retail MeetSound.bcsar (read-only)")
    ap.add_argument("--out", required=True, help="output directory")
    ap.add_argument("--tid", default=DEFAULT_TID, help="Luma title-ID folder")
    ap.add_argument("--romfs-rel", default=DEFAULT_ROMFS_REL,
                    help="romfs-relative destination path under the TID folder")
    args = ap.parse_args()

    src_path = Path(args.source)
    out_dir = Path(args.out)
    original = src_path.read_bytes()
    ar = Bcsar(original)

    # lift the four SE replicas from their own shared .bcseq files; each
    # carries its own INFO bank index (the update archive splits them)
    runs = {}
    for name in (SEQ_DRUM, SEQ_KEYFLY, SEQ_CURSOR, SEQ_SLIDE):
        se = ar.sound_by_name(name)
        se_stream, se_len = se.cseq_stream()
        runs[name] = walk_run(ar.data[se_stream:se_stream + se_len],
                              se.start_offset, name)
        runs[name].bank = se.bank_index
        print(f"  {name}: bank {se.bank_index} ({ar.bank_name(se.bank_index)}), "
              f"{len(runs[name].data)} B, {runs[name].ticks} ticks")
    if runs[SEQ_DRUM].bank != runs[SEQ_SLIDE].bank \
            or runs[SEQ_KEYFLY].bank != runs[SEQ_CURSOR].bank:
        raise ValueError("bank pairing changed; re-derive the track split")

    host_a = ar.sound_by_name(SEQ_HOST_A)
    host_b = ar.sound_by_name(SEQ_HOST_B)
    bat_a = assemble_a(runs[SEQ_DRUM], runs[SEQ_SLIDE], host_a.start_offset)
    bat_b = assemble_b(runs[SEQ_KEYFLY], runs[SEQ_CURSOR], host_b.start_offset)

    patched = bytearray(original)
    span_a = patch_track(patched, host_a, bat_a)
    span_b = patch_track(patched, host_b, bat_b)

    # self-checks: re-locate by name in the patched bytes, then re-parse
    # each rewritten payload command-by-command (exact-boundary termination)
    reparsed = Bcsar(bytes(patched))
    for name, bat, (s_abs, s_len) in ((SEQ_HOST_A, bat_a, span_a),
                                      (SEQ_HOST_B, bat_b, span_b)):
        chk = reparsed.sound_by_name(name)
        assert chk.volume == 127 and chk.bank_index == bat.bank
        check_payload(bytes(patched[s_abs:s_abs + s_len]))

    sd_path = out_dir / "sd" / "luma" / "titles" / args.tid / Path(args.romfs_rel)
    sd_path.parent.mkdir(parents=True, exist_ok=True)
    sd_path.write_bytes(patched)

    # ------------------------------------------------------------- manifest
    diffs = contiguous_diff_ranges(original, bytes(patched))
    labels = {}
    for host, bat in ((host_a, bat_a), (host_b, bat_b)):
        labels[host.volume_off] = f"INFO volume byte {host.volume} -> 127"
        labels[host.bank_field] = (f"INFO bank index {host.bank_index} -> "
                                   f"{bat.bank} ({ar.bank_name(bat.bank)})")
    unexpected = [s for s, e in diffs
                  if not (span_a[0] <= s < span_a[0] + span_a[1]
                          or span_b[0] <= s < span_b[0] + span_b[1]
                          or s in labels)]
    lines = [
        "# Capture-cartridge manifest",
        "",
        f"- source: `{src_path}`",
        f"- source sha256: `{hashlib.sha256(original).hexdigest()}`",
        f"- patched sha256: `{hashlib.sha256(bytes(patched)).hexdigest()}`",
        f"- track A host: **{SEQ_HOST_A}** (index {host_a.index}) — "
        f"{bat_a.tick / TICKS_PER_SEC:.2f} s/pass, {len(bat_a.buf)} B, "
        f"bank {bat_a.bank} ({ar.bank_name(bat_a.bank)})",
        f"- track B host: **{SEQ_HOST_B}** (index {host_b.index}) — "
        f"{bat_b.tick / TICKS_PER_SEC:.2f} s/pass, {len(bat_b.buf)} B, "
        f"bank {bat_b.bank} ({ar.bank_name(bat_b.bank)})",
        "- no 0xB6 in either stream (bank set via INFO only); both loop "
        "forever on hardware",
        "",
        "## Patched byte ranges",
        "",
        "| # | file offset | length | what |",
        "|---|---|---|---|",
    ]
    for n, (s, e) in enumerate(diffs, 1):
        if s in labels:
            what = labels[s]
        elif span_a[0] <= s < span_a[0] + span_a[1]:
            what = "track A .bcseq DATA payload"
        elif span_b[0] <= s < span_b[0] + span_b[1]:
            what = "track B .bcseq DATA payload"
        else:
            what = "UNEXPECTED"
        lines.append(f"| {n} | {s:#x} | {e - s} | {what} |")
    for track, bat in (("A", bat_a), ("B", bat_b)):
        lines += ["", f"## Track {track} event schedule (one pass)", "",
                  "| t (s) | section | event |", "|---|---|---|"]
        for tick, section, label in bat.events:
            lines.append(f"| {tick / TICKS_PER_SEC:7.3f} | {section} | {label} |")
    lines += ["", f"SD destination: `SD:/luma/titles/{args.tid}/{args.romfs_rel}`",
              "Enable Luma3DS \"Game patching\"; in the plaza music player, "
              f"track A replaces {SEQ_HOST_A} (\"Main Theme 1\") and track B "
              f"replaces {SEQ_HOST_B}.", ""]
    (out_dir / "MANIFEST.md").write_text("\n".join(lines), encoding="utf-8")

    if unexpected:
        print(f"FATAL: unexpected diffs at {[hex(s) for s in unexpected]}",
              file=sys.stderr)
        return 1
    print(f"cartridge OK: {sd_path}")
    print(f"track A pass = {bat_a.tick / TICKS_PER_SEC:.2f} s, "
          f"track B pass = {bat_b.tick / TICKS_PER_SEC:.2f} s, "
          f"{len(diffs)} patch ranges")
    return 0


if __name__ == "__main__":
    sys.exit(main())
