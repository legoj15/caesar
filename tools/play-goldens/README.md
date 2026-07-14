# play-goldens — the caesar-play render golden net (Net A)

`caesar-play` renders BCSAR sequences to PCM audio in a **separate** exe from the
shipped converter. The two nets that guard `caesar` cannot see it:

- **ab-verify** pins `caesar`'s SF2/MIDI/`.log`/stderr byte-for-byte across the
  corpus — but never runs `caesar-play`.
- **diag-goldens** pins `caesar`'s diagnostic surfaces — again, not the player.

The player's output is **audio**, invisible to both. This harness is its net: it
byte-pins the `sha256` of the rendered WAV for a fixed set of render invocations,
so a change that alters the player's sound (a DSP tweak, a sequencer fix, a
resampler change) is caught immediately, from the very first `.wav`.

This is the "stand up golden-hash comparison the moment the dry player emits its
first `.wav`" de-risk from the suite design (SUITE-DESIGN.md, risk 3). It is
**Net A** (deterministic golden hash). **Net B** — tolerance-band comparison
against fresh New 3DS console captures — is a later phase (needs the captures).

## What it proves

1. **Determinism.** Every render is run **twice** and must be byte-identical both
   times (capture *and* compare). A golden that isn't reproducible run-to-run is
   worthless; a run-to-run difference is a harness error (exit 2), never a pass.
   This is the same twice-run discipline diag-goldens applies to its
   heap-sensitive `-w` surfaces.
2. **Stability.** The rendered WAV's `sha256` matches the stored golden.

## Usage

```powershell
# First time, or after a DELIBERATE audio change (a new commit that should move
# the sound): snapshot the current exe's renders as the goldens.
tools\play-goldens\play-goldens.ps1 -Capture          # add -Force to overwrite

# Prove the current exe's renders are byte-identical to the goldens.
tools\play-goldens\play-goldens.ps1

# Prove the harness can actually detect a diff before trusting a PASS.
tools\play-goldens\play-goldens.ps1 -SelfTest
```

Exit codes (same contract as ab-verify / diag-goldens): `0` identical, `1` one or
more renders differ, `2` harness error (nothing verified — never a pass).

## What is pinned

The invocation list lives at the top of the script (`$Invocations`). Each entry
renders one WAV and pins its `sha256` + the process exit code. Current set:

| name             | mode            | source     | sequence                   | what it proves |
|------------------|-----------------|------------|----------------------------|----------------|
| `note-caravel`   | `--render-note` | caravel    | (bank of SE_CTR_COMMON_OK) | C2 single-voice DSP: one note at its root key |

(C3 adds the first real-sequence renders here.)

## What is NOT committed

Exactly like ab-verify and diag-goldens: **nothing corpus-derived**. The goldens
(WAV shas) and the manifest live under `%LOCALAPPDATA%\caesar-play-goldens`, never
in the repo. Only this script and README are checked in. Source archives are
verified by `sha256` before each run, so a golden can never silently drift onto a
different archive (the source moved out from under it → recapture with
`-Capture -Force`).

## Notes / limitations

- The renders are **dry**: no reverb/delay (stage 3), no native track/master
  volume yet (C6), so a dense polyphonic mix can clip on peaks — that is expected
  for the stage-2 dry sum and does not affect determinism.
- The stale-exe guard refuses to compare when any `src/` or CMake file is newer
  than the exe (a false PASS otherwise). Pass `-AllowStaleExe` only when you know
  the exe already contains the change.
