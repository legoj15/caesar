#!/usr/bin/env python3
"""Build the Surround **Part B** span-sweep stimulus cartridge.

Part A (../) proved on console that the NW4C `span` command (0xD7) is AUDIBLE
under System Settings "Surround" (front/rear virtualisation on the headphone
jack). Part B binds the opcode to its cause at the source: the per-voice DSP
`SourceConfiguration.gain[3][4]` quad gain matrix. This tool builds the
*stimulus* half — a patched MeetSound.bcsar that plays a clean sustained tone
while `span` (and, optionally, `front_bypass` 0xDB) sweeps in held steps long
enough for a one-shot GDB register snapshot per value. The *reader* half is
`gdb_read.py`; the live procedure is in `README.md`.

Same surgical LayeredFS mechanism as tools/capture-cartridge (v1/v2): two
StreetPass Mii Plaza music-player entries are hijacked in place, INFO volume ->
127 + INFO bank re-pointed, the embedded .bcseq DATA payload rewritten SAME-SIZE
(byte-identical stage-1 round-trip), NO 0xB6 (bank via INFO only — the stimulus
needs exactly one bank per track). Every probe is a crafted command stream on
program 23 (the shared 1.512 s sustained looping tone present in BOTH
BANK_MEET_SE_MAIN and BANK_MEET_LEGEND — the same clean instrument battery v2
used). Stdlib only. Never writes to the source path; output goes to --out.

Two tracks, deliberately a DIFFERENTIAL on the one open modelling question
(is `span` a continuous per-frame track control, or latched at note-on?):

  track A -> BGM_MAIN_Mii_Only_One, BANK_MEET_SE_MAIN
      HELD-NOTE span sweep. ONE sustained note (note-wait OFF so the stream
      keeps running while the voice sounds), then 0xD7 span steps 0/32/64/96/127
      each held --hold s. If the rear gain lanes track span across snapshots,
      `span` is a continuous control (the physically-expected NW4R/NW4C
      behaviour). This is the task's primary stimulus.

  track B -> BGM_DEN_EMPTY_LANDSCAPE, BANK_MEET_LEGEND
      RETRIGGER span sweep + front_bypass sweep. Part 1 re-attacks a FRESH note
      for each span value (so a reading is guaranteed even if `span` turns out
      to be latched at note-on). Part 2 sweeps front_bypass (0xDB) 0/64/127.

If track A's rear lanes move with span -> continuous (done). If A is flat but
B's do -> latched. Either way the operator gets the span -> gain[3][4] binding.

The gate (run automatically): the capture-cartridge Python self-checks
(locate-by-name re-parse + exact-boundary payload walk + INFO poke assert) AND,
if a built converter is found, the real `caesar-roundtrip --verify` on the
PATCHED archive (byte-identical stage-1 round-trip) plus a `caesar` parse.
"""

import argparse
import hashlib
import shutil
import subprocess
import sys
from pathlib import Path

# ---- reuse capture-cartridge's proven primitives (sibling tool) -------------
_CAP = Path(__file__).resolve().parents[2] / "capture-cartridge"
sys.path.insert(0, str(_CAP))
from build_cartridge import (            # noqa: E402
    Bcsar, enc_varint, check_payload, contiguous_diff_ranges, patch_track,
    TICKS_PER_SEC, SEQ_HOST_A, SEQ_HOST_B, DEFAULT_TID, DEFAULT_ROMFS_REL,
)

# ------------------------------------------------------------- probe constants

# Program 23 = the shared sustained looping tone (BANK_MEET_SE_MAIN sid7 /
# BANK_MEET_LEGEND sid9 are the same 1.512 s loop, rate 32728, root key 60).
# Verified by the bank/sample inspector in the battery-v2 work.
PILOT_PROG = 23
PILOT_KEY = 60
PILOT_VEL = 127
PILOT_LEN_TICKS = 96          # 1.0 s pilot at 96 ticks/s

BANK_SE_MAIN = "BANK_MEET_SE_MAIN"
BANK_LEGEND = "BANK_MEET_LEGEND"

# span (0xD7) ladder. 0 = full front, 64 ~ center, 127 = full rear (byte/63 ->
# 0.0-2.0, 1.0 = center). 32 & 96 discriminate the constant-power pan curve.
SPAN_LADDER = (0, 32, 64, 96, 127)
# front_bypass / main-dry send (0xDB) ladder — the optional second phase.
FB_LADDER = (0, 64, 127)

CMD_SPAN = 0xD7
CMD_FRONT_BYPASS = 0xDB


class SweepBattery:
    """One track's crafted sweep stream. `bank` is fixed for the whole battery
    (set via the hijacked INFO record, never 0xB6). Every hold is anchored with
    its tick + the active parameter value so the MANIFEST maps wall-clock time
    windows to span/front_bypass values for the one-shot reader."""

    def __init__(self, bank):
        self.buf = bytearray()
        self.tick = 0
        self.notewait = True          # engine default (noteWait ON)
        self.bank = bank
        # each hold: [t_start_tick, section, param, value, hold_ticks]
        self.holds = []

    # -- low-level emit -----------------------------------------------------
    def emit(self, data, adv=0):
        self.buf += bytes(data)
        self.tick += adv

    def rest(self, seconds):
        ticks = round(seconds * TICKS_PER_SEC)
        if ticks > 0:
            self.emit(b"\x80" + enc_varint(ticks), adv=ticks)

    def note(self, key, vel, length):
        # note-wait ON advances the tick by the gate length (blocks); OFF fires
        # the note-on and continues immediately (the voice still releases after
        # `length` ticks on its own).
        self.emit(bytes([key & 0x7F, vel & 0x7F]) + enc_varint(length),
                  adv=length if self.notewait else 0)

    def prg(self, n):
        self.emit(b"\x81" + enc_varint(n))

    def cc(self, op, arg):
        self.emit(bytes([op, arg & 0x7F]))

    def set_notewait(self, on):
        self.notewait = bool(on)
        self.emit(bytes([0xC7, 1 if on else 0]))

    def hold(self, section, param, value, hold_s):
        """Record a snapshot window at the CURRENT tick, then rest hold_s."""
        hold_ticks = round(hold_s * TICKS_PER_SEC)
        self.holds.append([self.tick, section, param, value, hold_ticks])
        self.rest(hold_s)

    # -- structure ----------------------------------------------------------
    def setup_and_pilot(self):
        # Deterministic state, then the pilot tone at tick 0 (identical bytes on
        # both tracks) so the operator can sync a stopwatch to it per session.
        self.emit(b"\xe1\x00\x78")   # tempo 120
        self.cc(0xC1, 127)           # track volume 127
        self.cc(0xC0, 64)            # pan center (isolate the front/rear axis)
        self.cc(0xD9, 0)             # reverb send OFF -> aux gain lanes stay 0,
        self.cc(0xDA, 0)             # chorus send OFF    so gain[0][*] alone
        #                              carries the span signal (clean read).
        self.prg(PILOT_PROG)
        self.note(PILOT_KEY, PILOT_VEL, PILOT_LEN_TICKS)   # blocks (notewait ON)
        self.rest(1.0)               # gap before the sweep

    def finish(self, loop_target):
        self.rest(2.0)
        self.emit(bytes([0x89]) + loop_target.to_bytes(3, "big"))   # loop back


def assemble_a(loop_target, hold_s):
    """Track A: HELD-NOTE span sweep (the primary, continuous-span stimulus)."""
    bat = SweepBattery(BANK_SE_MAIN)
    bat.setup_and_pilot()

    # One sustained note that outlasts the whole sweep. note-wait OFF so the
    # stream keeps running (emitting span steps + rests) while the voice sounds.
    sweep_ticks = len(SPAN_LADDER) * round(hold_s * TICKS_PER_SEC)
    note_ticks = sweep_ticks + PILOT_LEN_TICKS   # ~1 s tail past the last hold
    bat.prg(PILOT_PROG)
    bat.set_notewait(False)
    bat.note(PILOT_KEY, PILOT_VEL, note_ticks)   # note-on; does NOT advance tick
    for span in SPAN_LADDER:
        bat.cc(CMD_SPAN, span)
        bat.hold("A", "span", span, hold_s)
    bat.set_notewait(True)
    bat.finish(loop_target)
    return bat


def assemble_b(loop_target, hold_s):
    """Track B: RETRIGGER span sweep (latched-hedge) + front_bypass sweep."""
    bat = SweepBattery(BANK_LEGEND)
    bat.setup_and_pilot()

    bat.prg(PILOT_PROG)
    bat.set_notewait(False)

    # Part 1 — a FRESH note per span value: valid even if span latches at
    # note-on. Each note's gate length == the hold, so it rings the full window.
    hold_ticks = round(hold_s * TICKS_PER_SEC)
    for span in SPAN_LADDER:
        bat.cc(CMD_SPAN, span)
        bat.note(PILOT_KEY, PILOT_VEL, hold_ticks)   # retrigger
        bat.hold("B-span", "span", span, hold_s)
    bat.cc(CMD_SPAN, 64)                              # reset to center

    # Part 2 — front_bypass (0xDB) sweep, optional second phase.
    for fb in FB_LADDER:
        bat.cc(CMD_FRONT_BYPASS, fb)
        bat.note(PILOT_KEY, PILOT_VEL, hold_ticks)
        bat.hold("B-fb", "front_bypass", fb, hold_s)
    bat.cc(CMD_FRONT_BYPASS, 0)

    bat.set_notewait(True)
    bat.finish(loop_target)
    return bat


# ------------------------------------------------------------------- gate


def run_converter_gate(patched_path: Path, out_dir: Path):
    """Hard gate: the real caesar-roundtrip --verify (byte-identical stage-1
    round-trip of the PATCHED archive) + a caesar parse. Skipped with a clear
    notice if no built converter is found (the Python self-checks still ran)."""
    repo = Path(__file__).resolve().parents[3]
    rt = None
    conv = None
    for cand in ("Release", "RelWithDebInfo", "Debug"):
        p = repo / "build" / cand / "caesar-roundtrip.exe"
        c = repo / "build" / cand / "caesar.exe"
        if p.exists():
            rt, conv = p, (c if c.exists() else None)
            break
    # non-Windows fallback (no .exe suffix)
    if rt is None:
        for cand in ("Release", "RelWithDebInfo", "Debug", "."):
            p = repo / "build" / cand / "caesar-roundtrip"
            c = repo / "build" / cand / "caesar"
            if p.exists():
                rt, conv = p, (c if c.exists() else None)
                break
    if rt is None:
        print("  gate: no built caesar-roundtrip found -> converter gate "
              "SKIPPED (Python self-checks passed). Build build/Release to "
              "enable the hard stage-1 gate.")
        return True

    r = subprocess.run([str(rt), "--verify", str(patched_path)],
                       capture_output=True, text=True)
    summary = [ln for ln in r.stdout.splitlines() if ln.startswith("SUMMARY")]
    ok = r.returncode == 0 and all("mismatched=0" in ln for ln in summary) and summary
    for ln in summary:
        print(f"  gate roundtrip: {ln.split(chr(9), 1)[0]}  "
              f"{'  '.join(f for f in ln.split(chr(9)) if '=' in f)}")
    if not ok:
        print("  gate: ROUND-TRIP MISMATCH", file=sys.stderr)
        print(r.stdout[-2000:], file=sys.stderr)
        return False

    if conv is not None:
        cout = out_dir / "_gate_convert"
        cout.mkdir(parents=True, exist_ok=True)
        c = subprocess.run([str(conv), "-o", str(cout), str(patched_path)],
                           capture_output=True, text=True)
        if c.returncode != 0:
            print("  gate: caesar PARSE FAILED", file=sys.stderr)
            print(c.stdout[-2000:] + c.stderr[-2000:], file=sys.stderr)
            return False
        shutil.rmtree(cout, ignore_errors=True)
        print("  gate convert: caesar parsed the patched archive OK")
    print("  gate: PASS (byte-identical stage-1 round-trip + parse)")
    return True


# ------------------------------------------------------------------- manifest


def emit_manifest(out_dir, src_path, original, patched, ar, host_a, host_b,
                  bat_a, bat_b, span_a, span_b, tid, romfs_rel, hold_s):
    diffs = contiguous_diff_ranges(original, bytes(patched))
    labels = {}
    for host, bat in ((host_a, bat_a), (host_b, bat_b)):
        labels[host.volume_off] = f"INFO volume byte {host.volume} -> 127"
        labels[host.bank_field] = (f"INFO bank index {host.bank_index} -> "
                                   f"{bat.bank} ({ar.bank_name(bat.bank)})")

    lines = [
        "# Surround Part B — span-sweep stimulus MANIFEST",
        "",
        f"- source: `{src_path}`",
        f"- source sha256: `{hashlib.sha256(original).hexdigest()}`",
        f"- patched sha256: `{hashlib.sha256(bytes(patched)).hexdigest()}`",
        f"- hold per step: **{hold_s:.1f} s**",
        f"- track A host: **{SEQ_HOST_A}** (index {host_a.index}) — "
        f"{bat_a.tick / TICKS_PER_SEC:.2f} s/pass, {len(bat_a.buf)} B used / "
        f"{span_a[1] - host_a.start_offset} B window, bank {bat_a.bank} "
        f"({ar.bank_name(bat_a.bank)}) — HELD-NOTE span sweep",
        f"- track B host: **{SEQ_HOST_B}** (index {host_b.index}) — "
        f"{bat_b.tick / TICKS_PER_SEC:.2f} s/pass, {len(bat_b.buf)} B used / "
        f"{span_b[1] - host_b.start_offset} B window, bank {bat_b.bank} "
        f"({ar.bank_name(bat_b.bank)}) — RETRIGGER span + front_bypass sweep",
        "- pilot tone: prg 23, key 60, vel 127, 1.0 s, center, at t=0.000 on "
        "BOTH tracks — sync a stopwatch to the pilot each session.",
        "- both tracks LOOP FOREVER on hardware; times below are within one "
        "pass. To snapshot value V, wait until the stopwatch (mod the pass "
        "length) lands inside V's [t_start, t_end) window, then fire the "
        "one-shot GDB read (`gdb_read.py`).",
        "- reverb/chorus sends are 0, so aux gain lanes gain[1][*]/gain[2][*] "
        "stay 0 and gain[0][0..3] (MAIN front-L/R + rear-L/R) alone carries "
        "the span signal.",
        "- NO 0xB6 (bank via INFO only).",
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

    for letter, bat in (("A", bat_a), ("B", bat_b)):
        lines += ["", f"## Track {letter} time -> value map (one pass)", "",
                  "| t_start (s) | t_end (s) | section | param | value | "
                  "expected gain[3][4] effect |",
                  "|---|---|---|---|---|---|"]
        for t0, section, param, value, hold_ticks in bat.holds:
            t_start = t0 / TICKS_PER_SEC
            t_end = (t0 + hold_ticks) / TICKS_PER_SEC
            if param == "span":
                if value <= 8:
                    eff = "front: gain[0][0],[0][1] high; rear [0][2],[0][3]~0"
                elif value >= 120:
                    eff = "rear: gain[0][2],[0][3] high; front [0][0],[0][1]~0"
                else:
                    eff = "mid: front/rear both non-zero (constant-power split)"
                eff += " — in Stereo/Mono rear lanes stay 0 (cross-check)"
            else:
                eff = "front_bypass: watch gain[0][0],[0][1] (MAIN front L/R)"
            lines.append(f"| {t_start:7.3f} | {t_end:7.3f} | {section} | "
                         f"{param} | {value} | {eff} |")

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


# ------------------------------------------------------------------- driver


def bank_index_by_name(ar, name):
    for i in range(len(ar._category_records(ar.cbnk_table, 0x2206))):
        if ar.bank_name(i) == name:
            return i
    raise ValueError(f"bank not found: {name}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--source", required=True, help="retail MeetSound.bcsar (read-only)")
    ap.add_argument("--out", required=True, help="output directory")
    ap.add_argument("--hold", type=float, default=5.0,
                    help="seconds each span/front_bypass value is held (>=3-4 "
                         "recommended for a one-shot GDB read; default 5.0)")
    ap.add_argument("--tid", default=DEFAULT_TID, help="Luma title-ID folder")
    ap.add_argument("--romfs-rel", default=DEFAULT_ROMFS_REL,
                    help="romfs-relative destination path under the TID folder")
    ap.add_argument("--no-converter-gate", action="store_true",
                    help="skip the caesar-roundtrip/parse gate (Python "
                         "self-checks still run)")
    args = ap.parse_args()

    if args.hold < 2.0:
        print("FATAL: --hold must be >= 2 s for a usable snapshot window",
              file=sys.stderr)
        return 1

    src_path = Path(args.source)
    out_dir = Path(args.out)
    original = src_path.read_bytes()
    ar = Bcsar(original)

    host_a = ar.sound_by_name(SEQ_HOST_A)
    host_b = ar.sound_by_name(SEQ_HOST_B)

    bat_a = assemble_a(host_a.start_offset, args.hold)
    bat_b = assemble_b(host_b.start_offset, args.hold)
    bat_a.bank = bank_index_by_name(ar, BANK_SE_MAIN)
    bat_b.bank = bank_index_by_name(ar, BANK_LEGEND)

    # static schedule sanity (independent of the render)
    for letter, bat, want in (("A", bat_a, len(SPAN_LADDER)),
                              ("B", bat_b, len(SPAN_LADDER) + len(FB_LADDER))):
        ticks = [h[0] for h in bat.holds]
        assert ticks == sorted(ticks), f"track {letter}: holds not monotonic"
        assert len(bat.holds) == want, \
            f"track {letter}: {len(bat.holds)} holds != {want}"
        print(f"--- track {letter}: {len(bat.holds)} snapshot windows, "
              f"pass {bat.tick / TICKS_PER_SEC:.1f} s ---")

    patched = bytearray(original)
    span_a = patch_track(patched, host_a, bat_a)
    span_b = patch_track(patched, host_b, bat_b)

    # Python self-checks (reuse capture-cartridge): locate by name in the
    # patched bytes, assert the INFO poke, re-parse each payload to the byte.
    reparsed = Bcsar(bytes(patched))
    for name, bat, (s_abs, s_len) in ((SEQ_HOST_A, bat_a, span_a),
                                      (SEQ_HOST_B, bat_b, span_b)):
        chk = reparsed.sound_by_name(name)
        assert chk.volume == 127 and chk.bank_index == bat.bank, \
            f"{name}: INFO poke mismatch"
        check_payload(bytes(patched[s_abs:s_abs + s_len]))
    print("  self-check: locate-by-name + payload re-parse + INFO poke OK")

    sd_path = out_dir / "sd" / "luma" / "titles" / args.tid / Path(args.romfs_rel)
    sd_path.parent.mkdir(parents=True, exist_ok=True)
    sd_path.write_bytes(patched)

    unexpected, diffs = emit_manifest(out_dir, src_path, original, patched, ar,
                                      host_a, host_b, bat_a, bat_b, span_a,
                                      span_b, args.tid, args.romfs_rel, args.hold)
    if unexpected:
        print(f"FATAL: unexpected diffs at {[hex(s) for s in unexpected]}",
              file=sys.stderr)
        return 1

    if not args.no_converter_gate:
        if not run_converter_gate(sd_path, out_dir):
            print("FATAL: converter gate failed", file=sys.stderr)
            return 1

    print(f"\nPart B cartridge OK: {sd_path}")
    print(f"track A pass = {bat_a.tick / TICKS_PER_SEC:.2f} s "
          f"({len(bat_a.buf)}/{span_a[1] - host_a.start_offset} B), "
          f"track B pass = {bat_b.tick / TICKS_PER_SEC:.2f} s "
          f"({len(bat_b.buf)}/{span_b[1] - host_b.start_offset} B), "
          f"{len(diffs)} patch ranges; MANIFEST.md written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
