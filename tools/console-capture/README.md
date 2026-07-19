# console-capture — hands-free New-3DS audio capture harness

Turns the capture loop that has cost a human session each time — build → SD →
navigate → record → hand over WAVs — into a scriptable pipeline. Reuses the
`n3ds-mcp` server (`E:\GitHub\3ds-mcp`) **by import, never by copy**: input
injection (Rosalina InputRedirection), NTR screen capture, the Rosalina text
readout (the fork's command channel), and file-service transfer. The audio side
is `ffmpeg`/dshow off the Scarlett 2i2 line feed; the loop ends by handing the
captured WAV to `tools/console-tolerance` for a PASS/FAIL verdict.

This is the engine for the roadmap's *Console-in-the-loop automation* item.
**Live shakedown run 2026-07-18** — the volume override, the HOME→plaza
prefix, the plaza music-player path and the deploy semantics below are all
console-verified facts, not transcriptions.

## Pipeline stages

Each stage is independently callable on `ConsoleCaptureSession` and returns a
`StageResult` (truthy iff it succeeded, with a machine-readable `.data`):

| Stage | Method | What it does |
| --- | --- | --- |
| 1. preflight | `preflight()` | File service reachable + NTR stream healthy; re-applies the Rosalina volume-override pin **closed-loop against the text readout** (every press verified from the cursor text, the value read back from the `Value: [NN%]` row). |
| 2. deploy | `deploy(bcsar)` | **Verify-first**, straight over the file service (`/luma/titles/` content lane, port 4952 — **no ftpd**): `ftp_get` the remote back and sha256-compare — identical bytes = "already deployed, verified" with no write; differing bytes are pushed then re-verified. Refuses a >16 MiB cartridge **before** transferring, and names a missing LayeredFS directory (the lane has no MKD). |
| 3. navigate | `navigate(path, verifier)` | Data-driven plaza navigation; sends input + grabs an NTR frame per step, aborts at the first checkpoint the verifier rejects. |
| 4. record | `record(out, passes, ...)` | ffmpeg/dshow capture of N passes' seconds, then a **level/noise assertion** (peak window, noise floor, both channels present; loud fail on clipping/silence). |
| 5. verdict | `verdict(capture, render)` | Runs `console_tolerance.py`; maps exit 0/1/2 → PASS / out-of-tolerance / harness-error. |
| 6. perturbation A/B | `perturbation_ab(...)` | Records the same pass twice, NTR streaming **on** then **off**, and reports whether the audio moved beyond noise (proves the stream doesn't perturb the DAC). |

## Rig constants (calibration memory + live measurements 2026-07-18)

- Device `Analogue 1 + 2 (Focusrite USB Audio)`, 48 kHz / 16-bit stereo.
- ch0 = 3DS **LEFT**, ch1 = 3DS **RIGHT** (NOT reversed); knob mismatch +0.045 dB.
- **Rosalina volume override = 65 %** (`ROSALINA_VOLUME_OVERRIDE`), measured
  with the **Windows recording volume at 100 %** (which never needs touching
  again): battery-v2 pilot peaks −6.4 dBFS, channels matched ≤0.09 dB, zero
  clipping. Taper ≈0.35 dB/% around the operating point (70 % → −4.1, 60 % →
  −7.6). The override replaces the analog slider entirely.
- Peaks target ~−6 dBFS (±3 dB window); noise floor −67.8 dBFS-rms measured →
  assertion fails a quietest-window floor above −55 dBFS (hum guard).
- Battery pass: v1 ≈ 23.5 s both tracks; **battery v2: A ≈ 36.1 s, B ≈ 85.8 s**
  (`SECONDS_PER_PASS_V2`).

## Console facts the harness encodes (live-verified 2026-07-18)

- **The Rosalina volume walk**: open chord → root cursor resets to the top →
  `System configuration...` → `Control volume` → `Y` toggles the override
  (the `Value:` row only exists while ENABLED), DLEFT/DRIGHT step ±10,
  DUP/DDOWN ±1, `A` applies (screen shows `Success!`). Driven closed-loop by
  `RosalinaMenuDriver` over the readout; `get_screen` cannot see the overlay
  at all (the menu freezes the GPU), which is why the readout is the only eye.
- **Two input hazards around the overlay**: the open chord **leaks its DDOWN**
  into the app underneath (re-anchor navigation after any menu round-trip),
  and for ~1 s after the close pulse the resuming app **drops inputs**
  (`POST_MENU_CLOSE_DEADZONE_S` waits it out). The readout can also stall past
  its own retry budget during open/close transitions — the driver's tolerant
  reads absorb that.
- **HOME→plaza prefix**: saturate DLEFT (12×), then 2× DRIGHT — StreetPass
  Mii Plaza is the **3rd tile** in the 2026-07-19 layout (leftmost = NTR CFW,
  then Homebrew Utils, then Plaza; it was the 4th tile / 3× DRIGHT on 07-18).
  The HOME layout is **user-mutable** (tiles move as software is added/removed),
  so the DRIGHT count is best-effort — the top-screen **plaza-banner checkpoint
  is the real guard**, and a wrong count fails there loudly rather than
  launching the wrong tile. The checkpoint fires *before* the launching A. Fresh
  launch: splash + "Please wait" (~11 s) → "Careful!" modal ("Play"
  preselected) → announcement cards (ONE `A` each, count varies — 2 on the
  verified run; an extra `A` opens your own Mii's menu, `B` backs out).
- **Music Player**: press `Y` ("Switch Icons") to swap the bottom grid from
  StreetPass GAMES to plaza FEATURES — the games grid never contains it —
  then touch (52, 172) (row 3 col 1, the music note): touching a tile selects
  + names it on the top screen *without* launching; `A` opens it. Track list:
  Entrance / **Main Theme 1 = battery A** (1× DDOWN) / ...; battery B shows as
  "Find Mii - Dark Room" (DDOWN count not yet walked).
- **Start-state contract (v0)**: HOME menu with **no suspended software**.
  Close suspended software with HOME → `X` → `A`. Resuming a suspended plaza
  skips the splash/modals, which the encoded path does not model.
- **Deploy semantics** (hardware-verified 2026-07-19): the boot-time Rosalina
  file service (TCP 4952, on at boot in dev mode) now has a **STOR-only
  `/luma/titles/` content lane** (16 MiB ceiling), so `deploy` pushes the
  cartridge straight over it — **ftpd on :5000 is retired** (still reachable via
  `N3DS_FTP_PORT=5000`, but nothing requires it). STOR streams into
  `<target>.n3dstmp` and publishes by delete-then-rename, so a dropped link no
  longer destroys the installed cartridge; RETR (the verify read) is always
  allowed. Two lane facts `deploy` guards for you: **the 16 MiB ceiling is
  checked client-side before any bytes move** — an oversize STOR is otherwise
  only refused *after* streaming the whole file (≈149 s for 16 MiB; a slow STOR
  is not a hang) — and **there is no MKD**: the lane replaces bytes at an
  existing path only, so the LayeredFS directory tree must already exist on the
  SD; a STOR that fails on a missing parent directory is reported with that
  explicit cause. The content lane is also **overwrite-only forever** (STOR-only,
  no remote delete) — fine for a fixed cartridge path.
- **Deploy and NTR are mutually exclusive** (live-measured 2026-07-19): a
  sustained STOR **dies within seconds while NTR-HR is streaming** — a 6.1 MB
  push aborted at 8 KB, a 1 MB staging push at ~144 KB after 3 s, `WinError
  10053` both times; the same 6.1 MB push on a quiet radio (post-reboot, no NTR)
  finished in 45.6 s at 131 KB/s. Tiny (~16 B) STORs survive, and **RETR of a
  6 MB file works fine under NTR**. So `deploy` guards the *write* path: when
  bytes differ and a push is needed while the NTR stream is up, it **refuses
  fast** with a "stop the stream / reboot first" message instead of streaming a
  few KB and dying on a bare socket error. The verify-only path (identical bytes)
  still succeeds with the stream up, since RETR is unaffected. **Workflow
  implication:** deploy on a quiet radio — push the cartridge *before* bringing
  NTR up, or stop NTR / reboot, then deploy, then start streaming.
- **NTR stream is a single bind**: the console always streams video to host
  UDP 8001, so the harness and an interactive n3ds-mcp session can never both
  own frames. Standalone runs own the stream; running *alongside* an MCP
  session means injecting a stub NTR eye (`ntr_client=`) and letting the
  session verify screens independently — the underlying `NTRClient` is the
  identical class the MCP server runs. Related: **the 4951 command channel
  serves one reader at a time** — a concurrent `console_status` /
  `read_rosalina_menu` from the MCP session can starve the harness's readout
  for >1 s (live-observed). The driver's outer read retries absorb it, but
  don't query 4951 from the session while a volume walk is in flight.
- **Per-step screen sources (the `ScreenSource` seam)**: each `NavStep` names
  the eye that grabs its checkpoint frame via `NavStep.source`, defaulting to
  `"ntr"` (live video, non-invasive). It is a **per-step opt-in, never a global
  swap**. The one alternative is `"rosalina-screenshot"`: open the Rosalina
  menu → "Take screenshot" (dumps the frozen app framebuffers as uncompressed
  BMP to `/luma/screenshots/`, named `YYYY-MM-DD_HH-MM-SS.mmm_top.bmp` / `_bot`
  / `_top_right` when 3D is on) → close → fetch the BMP over the file service →
  decode (higher fidelity than the JPEG NTR stream; ~30 s/look). The file is
  found by **set difference** (list the dir before, take what *appeared* after),
  **not** newest-by-name — the directory accumulates litter stamped with bogus
  future dates (RTC weirdness, 2030/2031) that a name sort would pick first; if
  no new BMP appears, the eye returns `None` rather than guessing. It is driven
  blind by the same `RosalinaMenuDriver` over the 4951 readout the volume walk
  uses. Three constraints are **enforced as code guards**, not just prose:
  (a) opening the menu freezes the app, so the eye **refuses mid-record**
  (wired to the session's record state); (b) the open combo leaks its buttons
  into the app underneath (DDOWN is inert at the plaza card boundary; L/SELECT
  there are unverified), so a step must set **`menu_leak_safe=True`** before this
  eye may run at its position; (c) it drives the *shared* 4951 reader, so it
  relocates the single-bind exclusivity off NTR rather than removing it.
- **Sound-test profile (`SoundTestProfile`)**: everything true of the plaza but
  not of sound tests in general — the track→DDOWN map, the HOME→target tap
  prefix, the stop tap (`A` stops is a *plaza* law), the per-battery loop
  lengths — lives in `cfg.profile`. `PLAZA_PROFILE` is the default and the only
  profile that exists; it reproduces the pre-profile behavior exactly. Don't add
  other titles' profiles speculatively — fill one in only when its walk is
  actually verified on hardware.

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
from console_capture import (CaptureConfig, ConsoleCaptureSession,
                             SECONDS_PER_PASS_V2, plaza_path_for_track)

cfg = CaptureConfig()            # reads N3DS_IP from the environment
cfg.n3ds.ip = "192.168.0.71"
with ConsoleCaptureSession(cfg) as s:
    assert s.preflight()         # includes the readout-verified volume pin
    assert s.deploy(r"build\cartridge-v2\sd\luma\...\MeetSound.bcsar")
    assert s.navigate(plaza_path_for_track("Main Theme 1"))
    rec = s.record("BATTERY_A_console.wav", passes=2,
                   seconds_per_pass=SECONDS_PER_PASS_V2["A"])
    assert rec, rec.message
    s.stop_playback()
    print(s.verdict(rec.data["out"], r"...\PREDICTION_battery_A.wav").summary())
```

Thin CLI:

```
python console_capture.py --ip 192.168.0.71 preflight
python console_capture.py deploy path\to\MeetSound.bcsar
python console_capture.py navigate --track "Main Theme 1"
python console_capture.py record BATTERY_A.wav --passes 2 --seconds-per-pass 36.1
python console_capture.py verdict capture.wav render.wav
python console_capture.py ab --passes 1
```

## Tests

Fully offline — the n3ds-mcp simulators (`SimInputSink`/`SimNTR`), a
state-machine fake of the Rosalina overlay (`FakeRosalinaMenu`, which the REAL
`RosalinaMenuDriver` walks end-to-end), fake ftp + fake ffmpeg + canned verdict
runners. **No live console, no real ffmpeg.** Run with the n3ds-mcp venv Python
(it has pytest + Pillow + mcp + `n3ds_mcp`; the system Python lacks them):

```
E:\GitHub\3ds-mcp\.venv\Scripts\python.exe -m pytest tools\console-capture\tests -q
```

Covers: pass-count→seconds math, the ffmpeg command shape, the level assertion
(good / silence / clipping / dead-channel / too-hot / noisy-floor), **file-service
deploy** (identical-skip, hash-mismatch, titles-lane targeting, missing-directory
hint, generic lane hint, oversize client-side refusal, exactly-at-ceiling,
NTR-streaming write refusal + verify-only-still-works), navigation
checkpoint-failure abort, the live-verified path structure, the **`ScreenSource`
seam** (Rosalina-screenshot set-difference fetch that survives future-dated
litter + BMP decode, no-new-BMP → None, mid-record refusal, leak-unsafe refusal,
NTR default, per-step opt-in, unknown-source error), the **`SoundTestProfile`**
(plaza path reproduced byte-for-byte, constant composition, config default,
stop-tap send, no-stop-tap no-op), the volume walk (happy / arbitrary start /
close-on-failure / step law / screen parse), the verdict exit-code mapping, and
the NTR on/off perturbation A/B. **51 tests.**

## Open follow-ups

- **Screen-verification predicates — reference data.** The default
  `AcceptingVerifier` only asserts a frame arrived at each checkpoint (proves the
  stream is alive, no brittle pixels). The `RegionMeanVerifier` (region mean-RGB,
  tolerant) and the `"rosalina-screenshot"` eye are both wired offline; the
  missing piece is *reference frames* — every plaza `ExpectScreen` carries a
  prose-only `note`, no `mean_rgb`. **Needs console** to capture the reference
  BMPs (e.g. at the variable plaza card boundary), then set the hints. The
  offline `ScreenSource` seam prerequisite is **done**.
- **Battery B's DDOWN count** in the plaza track list. The existing counts were
  read with session eyes, not verified by tooling; fill B's in the same way on
  its next capture, ideally with a screen checkpoint on the highlighted row.
  **Needs console.**

## Notes

- `n3ds_mcp` is imported, not copied: works out of the box under the n3ds-mcp
  venv; otherwise found via `N3DS_MCP_SRC` or the sibling `..\3ds-mcp\src`.
- No numpy in-process — level analysis is stdlib `wave` (numpy is an optional
  accelerator only). The `console-tolerance` verdict runs as a subprocess, so its
  numpy dependency stays out of this harness.
