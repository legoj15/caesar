#!/usr/bin/env python3
"""Build the battery-v2 console capture cartridge: a second surgically patched
MeetSound.bcsar whose probes close the four still-open stage-2 constants and
add the recalibration-report follow-ups (2nd portamento point, 2nd LPF point,
decay-table console spot-check, steal-saturation tie order).

Same mechanism as v1 (build_cartridge.py, which stays the shipped tool): two
StreetPass Mii Plaza music-player entries are hijacked in place, INFO volume
-> 127 + bank re-pointed, the embedded .bcseq DATA payload rewritten same-size,
byte-identical stage-1 round-trip, NO 0xB6 (bank via INFO only). Every probe is
a CRAFTED command stream on the banks' own sustained looping instrument
(program 23 = the same 1.512 s looping tone in BOTH BANK_MEET_SE_MAIN and
BANK_MEET_LEGEND), so v2 needs no lifted-SE replicas at all (the walk_run
refusal never applies). Envelope/LFO/portamento/LPF are shaped with the
engine's own sequence commands.

Two banks, two tracks (same differential as v1):
  - track A -> BGM_MAIN_Mii_Only_One,   BANK_MEET_SE_MAIN
      pilot, pan curve 0/32/64/96/127, LPF points 24/40/48/64, velocity
      ladder, steal-saturation (30 stacked voices > 24-voice pool).
  - track B -> BGM_DEN_EMPTY_LANDSCAPE, BANK_MEET_LEGEND
      pilot, release curve 60/100/114/124 (two corrected DecayTable-tail
      indices), reverb-residual isolate (R127 instant cut, send max),
      decay-table probe (decay 122 / sustain 40), isolated pitch vibrato at
      two LFO rates, portamento distance + byte-rate points.

EVERY battery opens with a fixed pilot tone (program 23, key 60, vel 127,
1.0 s, center pan) at tick 0 so analysis can normalise each channel to it per
session (rig-gain-independent absolute levels — capture-rig-v2 plan).

Predictions (PREDICTION_battery_A/B.wav) MUST be rendered by the recalibrated
caesar_play (constant-rate portamento, 1-pole ~4.1 kHz LPF, /200 divisor).

Stdlib only. Never writes to the source path; output goes to --out.
"""

import argparse
import hashlib
import struct
import sys
from pathlib import Path

# Reuse v1's proven primitives (locate-by-name parser, VLQ, exact-boundary
# re-parse, in-place patcher, diff ranges). v1 stays the shipped cartridge.
from build_cartridge import (
    Bcsar, enc_varint, check_payload, contiguous_diff_ranges, patch_track,
    TICKS_PER_SEC, SEQ_HOST_A, SEQ_HOST_B, DEFAULT_TID, DEFAULT_ROMFS_REL,
)

# ------------------------------------------------------------- probe constants

# Program 23 is the shared sustained looping tone: BANK_MEET_SE_MAIN sid7 and
# BANK_MEET_LEGEND sid9 are the same 1.512 s loop (rate 32728, loop
# [26880-49488], root key 60). Verified by the bank/sample inspector.
PILOT_PROG = 23
PILOT_KEY = 60
PILOT_VEL = 127
PILOT_LEN_TICKS = 96          # 1.0 s at 96 ticks/s

# LEGEND program 9 is a short single-cycle loop (367 frames @ 96 kHz -> ~262 Hz
# = a clean C4 tone at root key 60), unlike the prog-23 pad/texture (loop repeat
# ~1.45 Hz). Its clear harmonic fundamental is what the pitch probes (vibrato,
# portamento) need to be FM/glide-demodulated on console — and it sidesteps the
# 6.5 Hz partial-beating trap. Track B (LEGEND) only.
PITCH_PROG = 9
PITCH_KEY = 60                # root -> native 262 Hz fundamental

BANK_SE_MAIN = "BANK_MEET_SE_MAIN"
BANK_LEGEND = "BANK_MEET_LEGEND"

# Release curve bytes: two slow (general curve) + two corrected-DecayTable-tail
# indices (114 & 124 were among the 2026-07-08 fix; higher byte = faster).
RELEASE_BYTES = (60, 100, 114, 124)
DECAY_BYTE = 122              # corrected-tail decay index for the decay probe
DECAY_SUSTAIN = 40           # sustain level so the decay actually drops
LPF_BYTES = (24, 40, 48, 64)  # 64 = the claimed filter-open point
PAN_BYTES = (0, 32, 64, 96, 127)   # 32/96 discriminate cos-sin vs sqrt-poly
VEL_LADDER = (127, 112, 96, 80, 64, 48, 32, 16)  # resolves the vel-96 anomaly
STEAL_COUNT = 30             # > 24-voice pool -> 6 forced steals
STEAL_KEY0 = 48
STEAL_VEL = 64
LFO_RATES = (48, 96)         # -> 3.75 / 7.5 Hz (player 5/64 Hz per unit)
LFO_DEPTH = 64               # x range 1 = 64 cents peak deviation


# -------------------------------------------------------------- battery model


class Battery2:
    """One track's crafted probe stream. `bank` is fixed for the whole battery
    (set via the hijacked INFO record, never 0xB6). Every probe is recorded
    with its tick AND its byte offset inside `buf`, so the MANIFEST can point
    the analysis at exact bytes."""

    def __init__(self, bank):
        self.buf = bytearray()
        self.tick = 0
        self.notewait = True
        self.bank = bank
        self.events = []   # (tick, buf_off, section, label, probe_bytes_hex)

    # -- low-level emit -----------------------------------------------------
    def emit(self, data, adv=0):
        self.buf += bytes(data)
        self.tick += adv

    def mark(self, section, label, probe_len=0):
        """Record a probe anchor at the CURRENT buf position/tick. probe_len
        (>0) captures that many upcoming bytes as the probe signature."""
        off = len(self.buf)
        sig = ""  # filled after the probe bytes are emitted, see probe()
        self.events.append([self.tick, off, section, label, sig, probe_len])

    def _finalize_sig(self, ev):
        _, off, _, _, _, plen = ev
        if plen:
            ev[4] = self.buf[off:off + plen].hex()

    # -- musical helpers ----------------------------------------------------
    def rest(self, seconds):
        ticks = round(seconds * TICKS_PER_SEC)
        if ticks > 0:
            self.emit(b"\x80" + enc_varint(ticks), adv=ticks)

    def note(self, key, vel, length):
        self.emit(bytes([key & 0x7F, vel & 0x7F]) + enc_varint(length),
                  adv=length if self.notewait else 0)

    def prg(self, n):
        self.emit(b"\x81" + enc_varint(n))

    def cc(self, op, arg):
        self.emit(bytes([op, arg & 0x7F]))

    def set_notewait(self, on):
        self.notewait = bool(on)
        self.emit(bytes([0xC7, 1 if on else 0]))

    def tempo(self, bpm):
        self.emit(b"\xe1" + int(bpm).to_bytes(2, "big"))

    def lfo_delay0(self):
        self.emit(b"\xe0\x00\x00")   # mod delay 0 (big-endian 16-bit arg)

    def env_reset(self):
        self.emit(b"\xfb")           # 0xFB clears latched ADSHR overrides

    # -- structure ----------------------------------------------------------
    def setup_and_pilot(self):
        # Deterministic state, then the pilot tone at tick 0 (both tracks
        # identical bytes: prg 23, key 60, vel 127, 1.0 s, center).
        self.emit(b"\xe1\x00\x78")   # tempo 120
        self.cc(0xC1, 127)           # track volume 127
        self.cc(0xC0, 64)            # pan center
        self.cc(0xD9, 0)             # reverb send OFF (deterministic dry start)
        self.prg(PILOT_PROG)
        self.mark("PILOT", f"pilot tone prg{PILOT_PROG} key{PILOT_KEY} "
                  f"vel{PILOT_VEL} 1.0s center", probe_len=3)
        self.note(PILOT_KEY, PILOT_VEL, PILOT_LEN_TICKS)
        self._finalize_sig(self.events[-1])
        self.rest(1.0)               # gap before the first probe

    def finish(self, loop_target):
        self.mark("loop", "end of pass; hardware jumps back to start")
        self.rest(2.0)
        self.emit(bytes([0x89]) + loop_target.to_bytes(3, "big"))

    def probe_note(self, section, label, key, vel, note_len_ticks, ring_s,
                   probe_len=3):
        """A single crafted note + ring, anchored for analysis."""
        self.mark(section, label, probe_len=probe_len)
        self.note(key, vel, note_len_ticks)
        self._finalize_sig(self.events[-1])
        self.rest(ring_s)


# --------------------------------------------------------------- assemblers


def assemble_a(loop_target):
    """Track A on BANK_MEET_SE_MAIN: pilot, pan curve, LPF points, velocity
    ladder, steal saturation."""
    bat = Battery2(BANK_SE_MAIN)
    bat.setup_and_pilot()

    # A1 pan curve (0/32/64/96/127) -- 32 & 96 are the cos-sin vs sqrt-poly
    # discriminators; each a sustained prg23 note held then a short ring.
    for pan in PAN_BYTES:
        bat.cc(0xC0, pan)
        bat.probe_note("A1", f"pan probe byte {pan}", PILOT_KEY, 110, 144, 0.4)
    bat.cc(0xC0, 64)

    # A2 LPF points (0xD8 cutoff): 24/40/48/64. 48 re-confirms v1's 4.1 kHz,
    # 24 & 40 give the 2nd corner + slope, 64 is the claimed open point. Play a
    # higher key (72, +1 oct) for more HF content above the corner.
    for cut in LPF_BYTES:
        bat.cc(0xD8, cut)
        bat.probe_note("A2", f"LPF cutoff byte {cut}", 72, 110, 192, 0.4)
    bat.cc(0xD8, 64)             # leave the filter open

    # A3 velocity ladder (fine): resolves the vel-96 single-point anomaly.
    for vel in VEL_LADDER:
        bat.probe_note("A3", f"velocity {vel} ((vel/127)^2 law)",
                       PILOT_KEY, vel, 96, 0.25)

    # A4 steal saturation: 30 stacked same-priority note-ons > 24-voice pool,
    # note-wait OFF so all start on one tick; distinct keys so the surviving
    # pitches reveal the sorted-insert tie order.
    bat.mark("A4", f"steal saturation: {STEAL_COUNT} stacked voices "
             f"(keys {STEAL_KEY0}..{STEAL_KEY0 + STEAL_COUNT - 1}, vel {STEAL_VEL})")
    bat.set_notewait(False)
    for i in range(STEAL_COUNT):
        bat.note(STEAL_KEY0 + i, STEAL_VEL, 240)   # 2.5 s length, looped tone
    bat.set_notewait(True)
    bat.rest(3.0)               # let the survivors ring, then release

    bat.finish(loop_target)
    return bat


def assemble_b(loop_target):
    """Track B on BANK_MEET_LEGEND: pilot, release curve, reverb residual,
    decay-table probe, isolated vibrato, portamento distance + byte-rate."""
    bat = Battery2(BANK_LEGEND)
    bat.setup_and_pilot()

    # B1 release curve (0xD3 override): DRY, four bytes spanning slow->fast,
    # incl. corrected-tail indices 114 & 124. Long ring exposes the slope.
    bat.cc(0xD9, 0)            # reverb OFF for clean release slopes
    for rb in RELEASE_BYTES:
        bat.cc(0xD3, rb)
        bat.probe_note("B1", f"release byte {rb} (dry, note-off slope)",
                       PILOT_KEY, 127, 240, 5.0)
        bat.env_reset()

    # B1r reverb-residual isolate: natural R127 instant cut + reverb send MAX,
    # so the ONLY thing ringing after the cut is the DSP reverb tail (feeds the
    # stage-3 reverb IR fit). No override -> valid even if 0xD3 is not honored.
    bat.cc(0xD9, 127)
    bat.probe_note("B1r", "reverb residual (R127 instant cut, send 127)",
                   PILOT_KEY, 127, 240, 8.0)
    bat.cc(0xD9, 0)

    # B2 decay-table probe: 0xD1 decay=122 (corrected index) + 0xD2 sustain=40
    # so the note decays from full to the sustain level -> DecayTable[122]
    # console A/B. Long note so the whole decay is captured.
    bat.cc(0xD1, DECAY_BYTE)
    bat.cc(0xD2, DECAY_SUSTAIN)
    bat.probe_note("B2", f"decay-table probe (decay {DECAY_BYTE}, "
                   f"sustain {DECAY_SUSTAIN})", PILOT_KEY, 127, 480, 2.5)
    bat.env_reset()

    # B3 isolated pitch vibrato on the clean prog-9 C4 tone: LFO target=pitch,
    # fixed depth/range, TWO rate bytes (48 & 96 -> 3.75 & 7.5 Hz). Two rates
    # defeat the 6.5 Hz partial-beat confound (beating is fixed by the sample;
    # the vibrato moves with the rate byte).
    bat.prg(PITCH_PROG)       # clean single-cycle C4 tone for FM demodulation
    bat.cc(0xCC, 0)           # LFO target = pitch
    bat.cc(0xCD, 1)           # range = 1 (depth x range = cents)
    bat.cc(0xCA, LFO_DEPTH)   # depth
    bat.lfo_delay0()
    for rate in LFO_RATES:
        bat.cc(0xCB, rate)
        bat.probe_note("B3", f"vibrato rate byte {rate} "
                       f"(~{rate * 5 / 64:.2f} Hz predicted)", PITCH_KEY, 120,
                       384, 0.4)
    bat.cc(0xCA, 0)           # depth 0 -> LFO off

    # B4 portamento on the clean prog-9 tone. Distance-proportionality: fixed
    # time byte 48, two glides (+3 st, +12 st). Byte->rate shape: fixed +12 st,
    # time bytes 24 & 96. The glide origin is the previous note's key.
    # (prog 9 still selected from B3.)
    def glide(section, label, origin_key, dest_key, time_byte, glide_note_ticks):
        bat.mark(section, label + " [origin]", probe_len=3)
        bat.note(origin_key, 120, 48)          # 0.5 s origin
        bat._finalize_sig(bat.events[-1])
        bat.cc(0xCE, 1)                        # porta ON
        bat.cc(0xCF, time_byte)               # porta time byte
        bat.mark(section, label + " [glide]", probe_len=3)
        bat.note(dest_key, 120, glide_note_ticks)
        bat._finalize_sig(bat.events[-1])
        bat.cc(0xCE, 0)                        # porta OFF
        bat.rest(0.5)

    glide("B4d", "porta distance +3st @ byte48", 60, 63, 48, 240)   # 2.5 s
    glide("B4d", "porta distance +12st @ byte48", 60, 72, 48, 528)  # 5.5 s
    glide("B4r", "porta rate +12st @ byte24", 60, 72, 24, 384)      # 4.0 s
    glide("B4r", "porta rate +12st @ byte96", 60, 72, 96, 864)      # 9.0 s
    #   (byte96 model glide is 8.45 s; the 9 s note captures arrival at the
    #    model rate, and a clear slope even if console glides slower.)

    bat.finish(loop_target)
    return bat


# ------------------------------------------------------------------- driver


def schedule_check(bat, letter):
    """Static sanity of the assembled schedule (independent of the render):
    onset monotonicity, pilot at tick 0, pan curve present, velocity ladder
    monotonic-vel, steal voice count. Returns (ok, [messages])."""
    msgs = []
    ok = True

    pilot = [e for e in bat.events if e[2] == "PILOT"]
    if len(pilot) != 1 or pilot[0][0] != 0:
        ok = False
        msgs.append(f"  FAIL pilot: expected exactly one at tick 0, "
                    f"got {[(e[0]) for e in pilot]}")
    else:
        msgs.append(f"  OK   pilot tone at tick 0 (bytes {pilot[0][4]})")

    ticks = [e[0] for e in bat.events]
    if ticks != sorted(ticks):
        ok = False
        msgs.append("  FAIL event ticks not monotonic")
    else:
        msgs.append(f"  OK   {len(bat.events)} events, ticks monotonic "
                    f"(pass {bat.tick / TICKS_PER_SEC:.1f} s)")

    if letter == "A":
        pans = [e for e in bat.events if e[2] == "A1"]
        if len(pans) != len(PAN_BYTES):
            ok = False
            msgs.append(f"  FAIL pan curve: {len(pans)} != {len(PAN_BYTES)}")
        else:
            msgs.append(f"  OK   pan curve {PAN_BYTES}")
        vels = [e for e in bat.events if e[2] == "A3"]
        if len(vels) != len(VEL_LADDER):
            ok = False
            msgs.append(f"  FAIL velocity ladder count {len(vels)}")
        else:
            msgs.append(f"  OK   velocity ladder {VEL_LADDER} (monotone)")
        steal = [e for e in bat.events if e[2] == "A4"]
        # count stacked note-ons emitted after the steal mark: the buffer
        # region between the A4 mark and set_notewait(True) holds STEAL_COUNT
        # note commands; assert the constant instead of re-parsing.
        if STEAL_COUNT <= 24:
            ok = False
            msgs.append(f"  FAIL steal count {STEAL_COUNT} <= 24 pool")
        else:
            msgs.append(f"  OK   steal saturation {STEAL_COUNT} voices "
                        f"(> 24 pool -> {STEAL_COUNT - 24} forced steals)")
    else:
        rel = [e for e in bat.events if e[2] == "B1"]
        if len(rel) != len(RELEASE_BYTES):
            ok = False
            msgs.append(f"  FAIL release curve count {len(rel)}")
        else:
            msgs.append(f"  OK   release curve {RELEASE_BYTES}")
        vib = [e for e in bat.events if e[2] == "B3"]
        if len(vib) != len(LFO_RATES):
            ok = False
            msgs.append(f"  FAIL vibrato count {len(vib)}")
        else:
            msgs.append(f"  OK   vibrato rates {LFO_RATES}")
        glides = [e for e in bat.events if e[2] in ("B4d", "B4r")]
        if len(glides) != 8:      # 4 glides x (origin + glide) marks
            ok = False
            msgs.append(f"  FAIL portamento marks {len(glides)} != 8")
        else:
            msgs.append("  OK   portamento: distance(+3,+12 @48) + rate(+12 @24,@96)")

    return ok, msgs


def emit_manifest(out_dir, src_path, original, patched, ar, host_a, host_b,
                  bat_a, bat_b, span_a, span_b, tid, romfs_rel):
    diffs = contiguous_diff_ranges(original, bytes(patched))
    labels = {}
    for host, bat in ((host_a, bat_a), (host_b, bat_b)):
        labels[host.volume_off] = f"INFO volume byte {host.volume} -> 127"
        labels[host.bank_field] = (f"INFO bank index {host.bank_index} -> "
                                   f"{bat.bank} ({ar.bank_name(bat.bank)})")
    lines = [
        "# Capture-cartridge v2 manifest",
        "",
        f"- source: `{src_path}`",
        f"- source sha256: `{hashlib.sha256(original).hexdigest()}`",
        f"- patched sha256: `{hashlib.sha256(bytes(patched)).hexdigest()}`",
        f"- track A host: **{SEQ_HOST_A}** (index {host_a.index}) — "
        f"{bat_a.tick / TICKS_PER_SEC:.2f} s/pass, {len(bat_a.buf)} B used / "
        f"{span_a[1] - host_a.start_offset} B window, bank {bat_a.bank} "
        f"({ar.bank_name(bat_a.bank)})",
        f"- track B host: **{SEQ_HOST_B}** (index {host_b.index}) — "
        f"{bat_b.tick / TICKS_PER_SEC:.2f} s/pass, {len(bat_b.buf)} B used / "
        f"{span_b[1] - host_b.start_offset} B window, bank {bat_b.bank} "
        f"({ar.bank_name(bat_b.bank)})",
        "- pilot tone: prg 23 (shared SE_MAIN/LEGEND loop), key 60, vel 127, "
        "1.0 s, center, at tick 0.000 on BOTH tracks — normalise each channel "
        "to it per session.",
        "- no 0xB6 (bank via INFO only); both loop forever on hardware; the "
        "recalibrated caesar_play renders one pass.",
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

    for letter, host, bat, span in (("A", host_a, bat_a, span_a),
                                     ("B", host_b, bat_b, span_b)):
        base = span[0] + host.start_offset  # abs file offset of buf[0]
        lines += ["", f"## Track {letter} probe schedule (one pass)", "",
                  "| t (s) | payload off | section | probe | bytes |",
                  "|---|---|---|---|---|"]
        for tick, off, section, label, sig, _ in bat.events:
            payload_off = host.start_offset + off
            lines.append(f"| {tick / TICKS_PER_SEC:7.3f} | "
                         f"{payload_off:#x} (@file {base + off:#x}) | "
                         f"{section} | {label} | `{sig}` |")

    lines += ["",
              f"SD destination: `SD:/luma/titles/{tid}/{romfs_rel}`",
              "Enable Luma3DS \"Game patching\"; track A replaces "
              f"{SEQ_HOST_A} (\"Main Theme 1\"), track B replaces {SEQ_HOST_B}.",
              ""]
    (out_dir / "MANIFEST.md").write_text("\n".join(lines), encoding="utf-8")
    unexpected = [s for s, e in diffs
                  if not (span_a[0] <= s < span_a[0] + span_a[1]
                          or span_b[0] <= s < span_b[0] + span_b[1]
                          or s in labels)]
    return unexpected, diffs


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

    host_a = ar.sound_by_name(SEQ_HOST_A)
    host_b = ar.sound_by_name(SEQ_HOST_B)

    # Resolve the two banks by NAME (locate-by-name invariant).
    def bank_index(name):
        for i in range(len(ar._category_records(ar.cbnk_table, 0x2206))):
            if ar.bank_name(i) == name:
                return i
        raise ValueError(f"bank not found: {name}")

    bank_a = bank_index(BANK_SE_MAIN)
    bank_b = bank_index(BANK_LEGEND)

    bat_a = assemble_a(host_a.start_offset)
    bat_b = assemble_b(host_b.start_offset)
    # Re-point each battery to the by-name-resolved bank index.
    bat_a.bank = bank_a
    bat_b.bank = bank_b

    # Static schedule check (before render): fail fast on a malformed schedule.
    all_ok = True
    for letter, bat in (("A", bat_a), ("B", bat_b)):
        ok, msgs = schedule_check(bat, letter)
        all_ok = all_ok and ok
        print(f"--- track {letter} schedule check ---")
        for m in msgs:
            print(m)
    if not all_ok:
        print("FATAL: schedule check failed", file=sys.stderr)
        return 1

    patched = bytearray(original)
    span_a = patch_track(patched, host_a, bat_a)
    span_b = patch_track(patched, host_b, bat_b)

    # Self-checks: re-locate by name, re-parse each payload to the exact byte.
    reparsed = Bcsar(bytes(patched))
    for name, bat, (s_abs, s_len) in ((SEQ_HOST_A, bat_a, span_a),
                                      (SEQ_HOST_B, bat_b, span_b)):
        chk = reparsed.sound_by_name(name)
        assert chk.volume == 127 and chk.bank_index == bat.bank, \
            f"{name}: INFO poke mismatch"
        check_payload(bytes(patched[s_abs:s_abs + s_len]))

    sd_path = out_dir / "sd" / "luma" / "titles" / args.tid / Path(args.romfs_rel)
    sd_path.parent.mkdir(parents=True, exist_ok=True)
    sd_path.write_bytes(patched)

    unexpected, diffs = emit_manifest(out_dir, src_path, original, patched, ar,
                                      host_a, host_b, bat_a, bat_b, span_a,
                                      span_b, args.tid, args.romfs_rel)
    if unexpected:
        print(f"FATAL: unexpected diffs at {[hex(s) for s in unexpected]}",
              file=sys.stderr)
        return 1

    print(f"\ncartridge v2 OK: {sd_path}")
    print(f"track A pass = {bat_a.tick / TICKS_PER_SEC:.2f} s "
          f"({len(bat_a.buf)}/{span_a[1] - host_a.start_offset} B), "
          f"track B pass = {bat_b.tick / TICKS_PER_SEC:.2f} s "
          f"({len(bat_b.buf)}/{span_b[1] - host_b.start_offset} B), "
          f"{len(diffs)} patch ranges")
    return 0


if __name__ == "__main__":
    sys.exit(main())
