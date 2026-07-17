# console-capture — hands-free New-3DS audio capture harness

Turns the capture loop that has cost a human session each time — build → SD →
navigate → record → hand over WAVs — into a scriptable pipeline. Reuses the
`n3ds-mcp` server (`E:\GitHub\3ds-mcp`) **by import, never by copy**: input
injection (Rosalina InputRedirection), NTR screen capture, and ftpd transfer.
The audio side is `ffmpeg`/dshow off the Scarlett 2i2 line feed; the loop ends by
handing the captured WAV to `tools/console-tolerance` for a PASS/FAIL verdict.

This is the offline-built engine for the roadmap's *Console-in-the-loop
automation* item. **The live shakedown is a separate step** — the main session,
which owns the physical console, runs it against real hardware and fills in the
two TODOs below.

## Pipeline stages

Each stage is independently callable on `ConsoleCaptureSession` and returns a
`StageResult` (truthy iff it succeeded, with a machine-readable `.data`):

| Stage | Method | What it does |
| --- | --- | --- |
| 1. preflight | `preflight()` | ftpd reachable + NTR stream healthy; re-applies the Rosalina volume-override pin (parameterised — see TODO). |
| 2. deploy | `deploy(bcsar)` | `ftp_put` the cartridge to the Luma path, `ftp_get` it back, **sha256 hash-compare** to confirm the push landed intact. |
| 3. navigate | `navigate(path, verifier)` | Data-driven plaza navigation; sends input + grabs an NTR frame per step, aborts at the first checkpoint the verifier rejects. |
| 4. record | `record(out, passes, ...)` | ffmpeg/dshow capture of N passes' seconds, then a **level/noise assertion** (peak window, noise floor, both channels present; loud fail on clipping/silence). |
| 5. verdict | `verdict(capture, render)` | Runs `console_tolerance.py`; maps exit 0/1/2 → PASS / out-of-tolerance / harness-error. |
| 6. perturbation A/B | `perturbation_ab(...)` | Records the same pass twice, NTR streaming **on** then **off**, and reports whether the audio moved beyond noise (proves the stream doesn't perturb the DAC). |

## Rig constants (from the `capture-rig-calibration` memory, 2026-07-15)

- Device `Analogue 1 + 2 (Focusrite USB Audio)`, 48 kHz / 16-bit stereo.
- ch0 = 3DS **LEFT**, ch1 = 3DS **RIGHT** (NOT reversed); knob mismatch +0.045 dB.
- Peaks target ~-6 dBFS (±3 dB window); noise floor -67.8 dBFS-rms measured →
  assertion fails a quietest-window floor above -55 dBFS (hum guard).
- Battery pass ≈ 23.5 s.

## The exact ffmpeg command

```
ffmpeg -hide_banner -loglevel error -nostdin -y \
  -f dshow -i audio=Analogue 1 + 2 (Focusrite USB Audio) \
  -t <seconds> -ac 2 -ar 48000 -c:a pcm_s16le <out.wav>
```

(`-ac/-ar/-c:a` pin dshow's default 48 kHz/16-bit stereo negotiation explicitly so
a driver-format surprise can't change the container.) Built by
`build_ffmpeg_cmd()`; `seconds = passes × seconds_per_pass (+ lead_in)`.

## Usage

Scriptable API (the primary deliverable):

```python
from console_capture import CaptureConfig, ConsoleCaptureSession, plaza_path_for_track

cfg = CaptureConfig()            # reads N3DS_IP from the environment
cfg.n3ds.ip = "192.168.0.71"
with ConsoleCaptureSession(cfg) as s:
    assert s.preflight()
    assert s.deploy(r"build\cartridge\...\MeetSound.bcsar")
    assert s.navigate(plaza_path_for_track("Main Theme 1"))
    rec = s.record("BATTERY_A_console.wav", passes=2)
    assert rec, rec.message
    s.stop_playback()
    print(s.verdict(rec.data["out"], r"...\PREDICTION_battery_A.wav").summary())
```

Thin CLI:

```
python console_capture.py --ip 192.168.0.71 preflight
python console_capture.py deploy path\to\MeetSound.bcsar
python console_capture.py navigate --track "Main Theme 1"
python console_capture.py record BATTERY_A.wav --passes 2
python console_capture.py verdict capture.wav render.wav
python console_capture.py ab --passes 1
```

## Tests

Fully offline — the n3ds-mcp simulators (`SimInputSink`/`SimNTR`) + fake ftp +
fake ffmpeg + canned verdict runners. **No live console, no real ffmpeg.** Run
with the n3ds-mcp venv Python (it has pytest + Pillow + mcp + `n3ds_mcp`; the
system Python lacks them):

```
E:\GitHub\3ds-mcp\.venv\Scripts\python.exe -m pytest tools\console-capture\tests -q
```

Covers: pass-count→seconds math, the ffmpeg command shape, the level assertion
(good / silence / clipping / dead-channel / too-hot / noisy-floor), deploy
hash-mismatch detection, navigation checkpoint-failure abort, the verdict
exit-code mapping, and the NTR on/off perturbation A/B. **26 tests.**

## What the live run must fill in (main session)

1. **Rosalina volume-override sequence + value.** `ROSALINA_VOLUME_OVERRIDE` is
   `None` (not yet measured) and `CaptureConfig.rosalina_volume_taps` is empty, so
   `preflight()` currently *skips* the re-apply with a clear notice instead of
   failing. Observe the Rosalina menu (chord `L+DDOWN+SELECT`) → volume-override
   slider path on the NTR stream, encode it as a `Tap` list into
   `rosalina_volume_taps`, and set `rosalina_volume` to the value that lands peaks
   in the target window. That value then becomes a rig constant in the
   calibration memory. (The camera-shutter path can silently reset the override —
   that's why preflight re-applies it every run.)
2. **HOME-menu → launch-plaza tap prefix.** `DEFAULT_HOME_TO_PLAZA_TAPS` is a
   placeholder (HOME + one A). The exact plaza-icon position (how many DRIGHT,
   then A) was never written down — capture it and pass it as `launch_taps`. The
   in-plaza path (modal-clear A's, DLEFT-to-Music-Player count, DDOWN track
   index) is encoded from the memory; confirm the `dleft_to_music_player` /
   `modal_clear_a_presses` counts on hardware (each is its own checkpoint, so a
   wrong count fails loudly at its step).
3. **Screen-verification predicates (optional but recommended).** The default
   `AcceptingVerifier` only asserts a frame arrived at each checkpoint (proves the
   stream is alive, no brittle pixels). For true screen identification, drop in
   `RegionMeanVerifier` (region mean-RGB, tolerant) or a template matcher via the
   `ExpectScreen.hints` interface, filling `hints["mean_rgb"]` / a template path
   per step from reference frames.

## Notes

- `n3ds_mcp` is imported, not copied: works out of the box under the n3ds-mcp
  venv; otherwise found via `N3DS_MCP_SRC` or the sibling `..\3ds-mcp\src`.
- No numpy in-process — level analysis is stdlib `wave` (numpy is an optional
  accelerator only). The `console-tolerance` verdict runs as a subprocess, so its
  numpy dependency stays out of this harness.
