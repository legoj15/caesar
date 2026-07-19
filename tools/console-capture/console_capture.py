#!/usr/bin/env python3
"""console_capture.py -- the hands-free New-3DS audio capture harness.

Turns the capture loop that has cost a human session each time (build -> SD ->
navigate -> record -> hand over WAVs) into a scriptable pipeline. Every stage is
independently callable and returns a :class:`StageResult` with an explicit
success/failure and a machine-readable ``data`` payload:

  1. preflight   -- file service reachable + NTR stream healthy, and re-apply
                    the Rosalina volume-override pin (closed-loop against the
                    Rosalina text readout; live-verified 2026-07-18).
  2. deploy      -- ftp_put a cartridge .bcsar to the Luma path, ftp_get it back
                    and hash-compare to confirm the push landed intact.
  3. navigate    -- screenshot-verified plaza navigation to a named track, driven
                    by InputRedirection input + NTR frame verification per step.
  4. record      -- ffmpeg/dshow capture of N passes' worth of seconds, then a
                    level/noise assertion (peak window, noise floor, both
                    channels present; fail loudly on clipping or silence).
  5. verdict     -- hand the WAV(s) to tools/console-tolerance for PASS/FAIL.
  6. perturb_ab  -- record the same pass twice, NTR streaming ON then OFF, and
                    report whether the audio moved beyond noise (proves the NTR
                    stream doesn't perturb the DAC).

This module REUSES ``n3ds_mcp`` (E:\\GitHub\\3ds-mcp) by import, never by copy:
InputClient (input_redirect), NTRClient (ntr), RosalinaReader (rosalina_read),
ftp_put/ftp_get/ftp_list (devtools), and Config (config). The only in-process
dependency beyond n3ds_mcp is the standard library; numpy is used ONLY as an
optional accelerator for the level analysis and is never required (the
console-tolerance verdict, which does need numpy, runs as a subprocess). Rig
constants and the plaza navigation path are transcribed from the
``capture-rig-calibration`` memory and live-verified 2026-07-18 (the volume
override, the HOME->plaza prefix and the Y/switch-icons music-player path).

OFFLINE-safe: nothing here sends to a live console until a stage method is
called with real clients. Every stage takes injectable dependencies so the whole
pipeline is exercisable against the n3ds-mcp simulators with no hardware.
"""

from __future__ import annotations

import hashlib
import math
import os
import re
import subprocess
import sys
import time
import wave
from array import array
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Optional, Protocol, Sequence


# --------------------------------------------------------------------------- #
# n3ds_mcp bootstrap -- reuse the server package by import, not by copy.
# --------------------------------------------------------------------------- #
def _bootstrap_n3ds_mcp() -> None:
    """Make ``n3ds_mcp`` importable.

    Works out of the box when this runs under the n3ds-mcp venv (the package is
    installed editable there). Otherwise it locates the source tree from
    ``N3DS_MCP_SRC`` or the conventional sibling checkout ``../3ds-mcp/src`` next
    to this repo, and inserts it on ``sys.path``. No copy of n3ds_mcp code lives
    in this repo -- it is always the single source at E:\\GitHub\\3ds-mcp.
    """
    try:
        import n3ds_mcp  # noqa: F401
        return
    except ImportError:
        pass
    candidates: list[Path] = []
    env = os.environ.get("N3DS_MCP_SRC")
    if env:
        candidates.append(Path(env))
    # tools/console-capture/console_capture.py -> repo root is parents[2]; the
    # n3ds-mcp checkout is a sibling of the caesar repo (E:\GitHub\{caesar,3ds-mcp}).
    repo_root = Path(__file__).resolve().parents[2]
    candidates.append(repo_root.parent / "3ds-mcp" / "src")
    for cand in candidates:
        if (cand / "n3ds_mcp" / "__init__.py").is_file():
            sys.path.insert(0, str(cand))
            return
    raise ImportError(
        "Could not import n3ds_mcp. Run under the n3ds-mcp venv "
        "(E:\\GitHub\\3ds-mcp\\.venv) or set N3DS_MCP_SRC to its src directory."
    )


_bootstrap_n3ds_mcp()

from n3ds_mcp import devtools  # noqa: E402
from n3ds_mcp.config import DEFAULT_FTP_PORT, Config, config_from_env  # noqa: E402
from n3ds_mcp.input_redirect import InputClient  # noqa: E402
from n3ds_mcp.ntr import NTRClient  # noqa: E402
from n3ds_mcp.rosalina_read import RosalinaReader, RosalinaScreen  # noqa: E402


# =========================================================================== #
# Rig constants (transcribed from the capture-rig-calibration memory, 2026-07-15)
# =========================================================================== #

# The dshow audio device the Scarlett 2i2 presents. Confirmed present on the dev
# machine; 48 kHz / 16-bit stereo is dshow's default negotiation.
AUDIO_DEVICE = "Analogue 1 + 2 (Focusrite USB Audio)"
SAMPLE_RATE = 48000
CHANNELS = 2

# The Luma LayeredFS path the capture-cartridge MeetSound.bcsar is pushed to
# (the plaza's v14 UPDATE romfs -- the base title path is a silent no-op).
LUMA_BCSAR_PATH = (
    "/luma/titles/0004001000021800/romfs/region_common/frame/sound/MeetSound.bcsar"
)

# Level calibration (memory: slider set so peaks land ~-6.2 dBFS; noise floor
# -67.8 dBFS-rms at 48 kHz/16-bit dshow). ch0 = 3DS LEFT, ch1 = 3DS RIGHT (NOT
# reversed); inter-channel knob mismatch +0.045 dB (negligible).
TARGET_PEAK_DBFS = -6.0
PEAK_MARGIN_DB = 3.0            # accept peaks in [target-margin, target+margin]
CLIP_DBFS = -0.10              # a channel peaking at/above this reads as clipping
SILENCE_DBFS = -40.0          # a channel below this on peak reads as absent/silent
NOISE_FLOOR_MAX_DBFS = -55.0  # quietest-window RMS must sit below this (hum guard;
                              # measured floor -67.8 gives ~13 dB headroom)
KNOB_MISMATCH_DB = 0.045      # constant L/R gain offset; subtract only if needed

# One battery pass, seconds, per cartridge generation. v1: both tracks ~23.5 s
# (roadmap: "passes exactly 23.50 s apart"). Battery v2 lengthened the tracks:
# A ~36.1 s, B ~85.8 s (console-confirmed loop periods 35.9 / 85.79 s) -- the
# CLI resolves these via `record --battery A|B`; API callers pass
# seconds_per_pass=SECONDS_PER_PASS_V2[letter].
SECONDS_PER_PASS = 23.5
SECONDS_PER_PASS_V2 = {"A": 36.1, "B": 85.8}

# --- Rosalina volume override (rig v2; console-measured 2026-07-18) --------- #
# The digitally-exact volume override that replaces the frozen analog slider.
# Measured live (plaza music player, battery-v2 track A -- its vel-127 pilot is
# the loudest battery element -- with the Windows recording volume at 100%,
# which now never needs touching):
#     override 70% -> pilot peak -4.10 dBFS
#     override 65% -> pilot peak -6.39 dBFS   <-- chosen (target -6.0 +/- 3.0)
#     override 60% -> pilot peak -7.60 dBFS
# Taper ~0.35 dB/% in this region (linear-in-dB); channels matched to <=0.09 dB;
# astats flat factor 0 at every point (no analog clipping). The camera-shutter
# path can silently reset the override, which is exactly why the capture
# preflight re-applies it every run.
ROSALINA_VOLUME_OVERRIDE: Optional[int] = 65

# The overlay menu walk (Luma3DS v13.5, n3ds-mcp fork; verified live): the open
# chord brings up the root menu with the cursor RESET to the top entry, then
# "System configuration..." -> A lands the cursor on "Control volume" -> A opens
# the volume screen ("Y: Toggle volume slider override", "Current status:",
# "Value: [NN%]"). Y enables the override, DLEFT/DRIGHT step the value +/-10 and
# DUP/DDOWN +/-1, A applies (the screen shows "Success!"). The walk is driven
# CLOSED-LOOP against the Rosalina text readout (RosalinaReader, the fork's UDP
# command channel): every press is confirmed from the actual cursor/row text and
# the applied value is read back from the "Value:" row -- which retires both the
# old blind-tap-list design and its "verify via an NTR frame region" TODO
# (get_screen cannot see the overlay at all; the menu freezes the GPU).
ROSALINA_SYSCONFIG_ENTRY = "System configuration..."
ROSALINA_VOLUME_ENTRY = "Control volume"

# Two live-measured input hazards around the overlay (2026-07-18):
#   * the open chord LEAKS its DDOWN into the app underneath (the plaza music
#     player's list cursor moved down one entry) -- navigation state must be
#     re-anchored after any menu round-trip, never assumed. The preflight runs
#     while the console sits on the HOME menu, where the leak is harmless, and
#     the HOME->plaza prefix re-anchors with a DLEFT saturation anyway.
#   * for ~1 s after the close pulse the resuming app DROPS inputs (a play tap
#     vanished without a trace); POST_MENU_CLOSE_DEADZONE_S waits it out.
POST_MENU_CLOSE_DEADZONE_S = 1.2


# =========================================================================== #
# Data model
# =========================================================================== #

@dataclass
class StageResult:
    """The outcome of one pipeline stage. Truthy iff the stage succeeded."""

    stage: str
    ok: bool
    message: str = ""
    data: dict = field(default_factory=dict)

    def __bool__(self) -> bool:
        return self.ok

    def summary(self) -> str:
        tag = "OK  " if self.ok else "FAIL"
        return f"[{tag}] {self.stage}: {self.message}"


@dataclass
class Tap:
    """One input tap in a navigation step. ``buttons`` may include HID buttons
    (A, DLEFT, DDOWN, ...) and/or interface strings (HOME, POWER) -- InputClient
    routes each to the correct wire field. ``touch`` is an optional bottom-screen
    (x, y) tap sent BEFORE the buttons (the plaza feature grid is selected by
    touch: touching a tile selects + names it on the top screen without
    launching). ``wait_ms`` is the settle delay AFTER the release, before the
    next tap (menu animations)."""

    buttons: tuple[str, ...]
    hold_ms: int = 80
    wait_ms: int = 350
    touch: Optional[tuple[int, int]] = None


@dataclass
class ExpectScreen:
    """A checkpoint assertion hook passed to the pluggable screen verifier.

    ``label`` names the expected screen; ``screen`` selects top/bottom; ``region``
    is an optional (x, y, w, h) ROI hint; ``hints`` carries verifier-specific data
    (a template path, an expected mean-colour range, etc.). Deliberately generic
    so a real verifier can be dropped in without touching the navigation data.
    """

    label: str
    screen: str = "top"
    region: Optional[tuple[int, int, int, int]] = None
    hints: dict = field(default_factory=dict)


@dataclass
class NavStep:
    """One navigation step: run ``taps``, wait ``settle_ms``, grab a frame for
    ``expect.screen`` and hand (step, frame) to the verifier."""

    name: str
    taps: tuple[Tap, ...]
    expect: Optional[ExpectScreen] = None
    settle_ms: int = 500


@dataclass
class LevelReport:
    """Per-channel level metrics from one captured WAV (dBFS)."""

    sample_rate: int
    channels: int
    peak_dbfs: list[float]
    rms_dbfs: list[float]
    noise_floor_dbfs: list[float]
    clipped_fraction: list[float]
    duration_s: float

    def channel_present(self, ci: int, silence_dbfs: float = SILENCE_DBFS) -> bool:
        return ci < self.channels and self.peak_dbfs[ci] > silence_dbfs


@dataclass
class CaptureConfig:
    """Everything the harness needs, composed over the n3ds-mcp Config.

    The n3ds ``Config`` carries the console IP + ports (input UDP 4950, the
    fork's command/readout channel UDP 4951, NTR control TCP 8000, video UDP
    8001, and the file service TCP 4952 -- the Rosalina file service is on at
    boot in dev mode; legacy ftpd needs N3DS_FTP_PORT=5000). The
    capture-specific rig constants default from the calibration memory and are
    all overridable.
    """

    n3ds: Config = field(default_factory=config_from_env)

    audio_device: str = AUDIO_DEVICE
    sample_rate: int = SAMPLE_RATE
    channels: int = CHANNELS

    luma_bcsar_path: str = LUMA_BCSAR_PATH
    captures_dir: Path = field(default_factory=lambda: Path.cwd() / "captures")

    seconds_per_pass: float = SECONDS_PER_PASS
    lead_in_s: float = 0.0

    # level assertion thresholds
    target_peak_dbfs: float = TARGET_PEAK_DBFS
    peak_margin_db: float = PEAK_MARGIN_DB
    clip_dbfs: float = CLIP_DBFS
    silence_dbfs: float = SILENCE_DBFS
    noise_floor_max_dbfs: float = NOISE_FLOOR_MAX_DBFS
    ab_level_tol_db: float = 1.0   # perturbation A/B: max |dRMS|,|dPeak| per channel

    # Rosalina volume override (rig constant, measured 2026-07-18). None
    # disables the re-apply (the preflight then SKIPS it with a notice).
    rosalina_volume: Optional[int] = ROSALINA_VOLUME_OVERRIDE

    # external tools
    ffmpeg_exe: str = "ffmpeg"
    python_exe: str = field(default_factory=lambda: sys.executable)
    console_tolerance_py: Path = field(
        default_factory=lambda: Path(__file__).resolve().parents[1]
        / "console-tolerance"
        / "console_tolerance.py"
    )


# =========================================================================== #
# WAV level analysis (stdlib; numpy is an optional accelerator only)
# =========================================================================== #

def _dbfs(amp: float) -> float:
    """Normalised amplitude (0..1) -> dBFS, floored at -120 dB for silence."""
    return -120.0 if amp <= 1e-9 else 20.0 * math.log10(amp)


def _read_wav_channels(path: str | Path):
    """Decode a PCM WAV into (rate, sampwidth, [channel arrays], full_scale).

    Handles 8/16/32-bit PCM (the ffmpeg capture forces pcm_s16le). Channels are
    returned de-interleaved as ``array`` objects of signed ints centred on 0.
    """
    with wave.open(str(path), "rb") as w:
        nch = w.getnchannels()
        sw = w.getsampwidth()
        rate = w.getframerate()
        nframes = w.getnframes()
        raw = w.readframes(nframes)
    if sw == 2:
        samples = array("h")
        samples.frombytes(raw)
        scale = 32768.0
    elif sw == 4:
        samples = array("i")
        samples.frombytes(raw)
        scale = 2147483648.0
    elif sw == 1:
        # 8-bit WAV PCM is UNSIGNED, offset 128; recentre to signed.
        u = array("B")
        u.frombytes(raw)
        samples = array("h", (b - 128 for b in u))
        scale = 128.0
    else:
        raise ValueError(f"unsupported PCM sample width {sw} bytes in {path}")
    channels = [samples[c::nch] for c in range(nch)] if nch else []
    return rate, sw, channels, scale


def _channel_metrics(ch, scale: float, rate: int, window_ms: float = 50.0):
    """Peak dBFS, RMS dBFS, noise-floor dBFS, clipped-fraction for one channel.

    The noise floor is the MINIMUM per-window RMS -- the quietest sustained
    moment. A real battery capture always has quiet regions (the 1 s silent
    lead-in + inter-note gaps), so the minimum reads the true analog floor
    (~-67.8 dBFS here) while a ground-loop hum lifts even the quietest window
    above threshold. (A percentile would false-fail a gapless dense loop, which
    has no quiet window at all.) Uses numpy if importable (fast path) and a
    pure-Python fallback otherwise; both give the same numbers, so tests exercise
    the fallback and real captures get the speed.
    """
    n = len(ch)
    if n == 0:
        return -120.0, -120.0, -120.0, 0.0
    win = max(1, int(window_ms / 1000.0 * rate))
    clip_thresh = scale - 1.0  # a sample at the rail

    try:
        import numpy as np  # optional accelerator

        a = np.asarray(ch, dtype=np.float64)  # array.array -> float64 via buffer
        peak = float(np.max(np.abs(a))) / scale
        rms = math.sqrt(float(np.mean(a * a))) / scale
        clipped = float(np.count_nonzero(np.abs(a) >= clip_thresh)) / n
        nwin = n // win
        if nwin >= 2:
            trimmed = a[: nwin * win].reshape(nwin, win)
            wrms = np.sqrt(np.mean(trimmed * trimmed, axis=1)) / scale
            floor = float(np.min(wrms))
        else:
            floor = rms
    except ImportError:
        peak_amp = max(max(ch), -min(ch))
        peak = peak_amp / scale
        ss = 0.0
        clip_n = 0
        for s in ch:
            f = float(s)
            ss += f * f
            if f >= clip_thresh or f <= -clip_thresh:
                clip_n += 1
        rms = math.sqrt(ss / n) / scale
        clipped = clip_n / n
        wrms: list[float] = []
        nwin = n // win
        for i in range(nwin):
            seg = ch[i * win : (i + 1) * win]
            s2 = 0.0
            for s in seg:
                s2 += float(s) * float(s)
            wrms.append(math.sqrt(s2 / win) / scale)
        floor = min(wrms) if len(wrms) >= 2 else rms

    return _dbfs(peak), _dbfs(rms), _dbfs(floor), clipped


def analyze_levels(path: str | Path) -> LevelReport:
    """Full per-channel :class:`LevelReport` for a captured WAV."""
    rate, _sw, channels, scale = _read_wav_channels(path)
    peaks, rmss, floors, clips = [], [], [], []
    dur = 0.0
    for ch in channels:
        p, r, f, c = _channel_metrics(ch, scale, rate)
        peaks.append(p)
        rmss.append(r)
        floors.append(f)
        clips.append(c)
        dur = max(dur, len(ch) / float(rate) if rate else 0.0)
    return LevelReport(
        sample_rate=rate,
        channels=len(channels),
        peak_dbfs=peaks,
        rms_dbfs=rmss,
        noise_floor_dbfs=floors,
        clipped_fraction=clips,
        duration_s=dur,
    )


def assert_capture_levels(report: LevelReport, cfg: CaptureConfig) -> StageResult:
    """The level/noise assertion: peak in the target window, noise floor below
    threshold, both channels present; loud fail on clipping or silence."""
    reasons: list[str] = []
    lo = cfg.target_peak_dbfs - cfg.peak_margin_db
    hi = cfg.target_peak_dbfs + cfg.peak_margin_db

    if report.channels < 2:
        reasons.append(f"not stereo ({report.channels} channel(s)); both channels required")

    for ci in range(report.channels):
        tag = "L" if ci == 0 else ("R" if ci == 1 else f"ch{ci}")
        peak = report.peak_dbfs[ci]
        clipped = report.clipped_fraction[ci]
        floor = report.noise_floor_dbfs[ci]
        if clipped > 1e-4 or peak >= cfg.clip_dbfs:
            reasons.append(f"{tag} clipping (peak {peak:.1f} dBFS, {clipped*100:.3f}% at rail)")
            continue
        if peak <= cfg.silence_dbfs:
            reasons.append(f"{tag} silent/absent (peak {peak:.1f} dBFS)")
            continue
        if not (lo <= peak <= hi):
            reasons.append(f"{tag} peak {peak:.1f} dBFS outside window [{lo:.1f}, {hi:.1f}]")
        if floor > cfg.noise_floor_max_dbfs:
            reasons.append(f"{tag} noise floor {floor:.1f} dBFS above {cfg.noise_floor_max_dbfs:.1f}")

    data = {
        "peak_dbfs": report.peak_dbfs,
        "rms_dbfs": report.rms_dbfs,
        "noise_floor_dbfs": report.noise_floor_dbfs,
        "clipped_fraction": report.clipped_fraction,
        "duration_s": report.duration_s,
        "window": [lo, hi],
    }
    if reasons:
        return StageResult("levels", False, "; ".join(reasons), data)
    peaks = ", ".join(f"{p:.1f}" for p in report.peak_dbfs)
    return StageResult("levels", True, f"levels OK (peaks {peaks} dBFS in [{lo:.1f},{hi:.1f}])", data)


# =========================================================================== #
# ffmpeg / dshow recorder
# =========================================================================== #

def build_ffmpeg_cmd(cfg: CaptureConfig, seconds: float, out_path: str | Path) -> list[str]:
    """The exact ffmpeg dshow command, as an argv list (no shell).

    -f dshow -i audio=<device> captures the Scarlett line feed; -t bounds the
    take; -ac/-ar/-c:a force 48 kHz 16-bit stereo (dshow's default negotiation,
    pinned explicitly so a driver-format surprise can't change the container).
    """
    return [
        cfg.ffmpeg_exe,
        "-hide_banner",
        "-loglevel", "error",
        "-nostdin",
        "-y",
        "-f", "dshow",
        "-i", f"audio={cfg.audio_device}",
        "-t", f"{seconds:.3f}",
        "-ac", str(cfg.channels),
        "-ar", str(cfg.sample_rate),
        "-c:a", "pcm_s16le",
        str(out_path),
    ]


class Recorder(Protocol):
    """A pluggable audio recorder so tests inject a fake ffmpeg."""

    def record(self, cfg: CaptureConfig, seconds: float, out_path: str | Path) -> None:
        ...


class FfmpegRecorder:
    """Default recorder: spawns ffmpeg/dshow and waits for the take to finish."""

    def record(self, cfg: CaptureConfig, seconds: float, out_path: str | Path) -> None:
        cmd = build_ffmpeg_cmd(cfg, seconds, out_path)
        # ffmpeg runs for ~`seconds`; give it a generous margin to flush + exit.
        timeout = seconds + 30.0
        try:
            proc = subprocess.run(
                cmd, capture_output=True, text=True, timeout=timeout
            )
        except FileNotFoundError as exc:
            raise RuntimeError(
                f"ffmpeg not found ({cfg.ffmpeg_exe}). Install ffmpeg and ensure "
                f"it is on PATH, or set CaptureConfig.ffmpeg_exe."
            ) from exc
        except subprocess.TimeoutExpired as exc:
            raise RuntimeError(
                f"ffmpeg capture timed out after {timeout:.0f}s (device "
                f"'{cfg.audio_device}' -- is it present and free?)."
            ) from exc
        if proc.returncode != 0 or not Path(out_path).is_file():
            raise RuntimeError(
                f"ffmpeg capture failed (exit {proc.returncode}). Is the device "
                f"'{cfg.audio_device}' present?\n{proc.stderr.strip()}"
            )


def record_seconds(pass_count: int, seconds_per_pass: float, lead_in_s: float = 0.0) -> float:
    """Pass-count -> capture seconds. Pure so the arithmetic is directly tested."""
    if pass_count < 1:
        raise ValueError("pass_count must be >= 1")
    return lead_in_s + pass_count * seconds_per_pass


# =========================================================================== #
# Screen verification (pluggable predicate over an NTR frame)
# =========================================================================== #

class ScreenVerifier(Protocol):
    """Verifies that a captured frame matches a navigation checkpoint.

    Returns (ok, note). The frame is a PIL ``Image`` (upright) or None if no
    frame was available. A real implementation does template/region matching;
    the default only asserts the stream produced a frame at the checkpoint.
    """

    def verify(self, step: NavStep, frame) -> tuple[bool, str]:
        ...


class AcceptingVerifier:
    """Default verifier: a checkpoint passes iff a frame was actually captured.

    This proves the NTR stream is alive and the console is responding at each
    step (the single most common live failure), WITHOUT brittle pixel checks.
    Drop in a template/region verifier for true screen identification -- the
    ExpectScreen hints (label / region / template path) are the interface for it.
    """

    def verify(self, step: NavStep, frame) -> tuple[bool, str]:
        label = step.expect.label if step.expect else step.name
        if frame is None:
            return False, f"no frame at checkpoint '{label}'"
        return True, f"frame present at '{label}' (screen identity unchecked -- stub verifier)"


class RegionMeanVerifier:
    """Example concrete verifier: checks a region's mean RGB against an expected
    range from ``expect.hints['mean_rgb']`` = ((rlo,rhi),(glo,ghi),(blo,bhi)).

    A tolerant mean over a region -- not a per-pixel template -- so JPEG noise and
    small layout shifts don't cause false negatives. Falls back to AcceptingVerifier
    behaviour when no ``mean_rgb`` hint is present on the step.
    """

    def __init__(self, fallback: Optional[ScreenVerifier] = None) -> None:
        self._fallback = fallback or AcceptingVerifier()

    def verify(self, step: NavStep, frame) -> tuple[bool, str]:
        exp = step.expect
        if exp is None or "mean_rgb" not in exp.hints:
            return self._fallback.verify(step, frame)
        if frame is None:
            return False, f"no frame at checkpoint '{exp.label}'"
        img = frame.convert("RGB")
        if exp.region:
            x, y, w, h = exp.region
            img = img.crop((x, y, x + w, y + h))
        px = list(img.getdata())
        if not px:
            return False, f"empty region at '{exp.label}'"
        n = len(px)
        means = (
            sum(p[0] for p in px) / n,
            sum(p[1] for p in px) / n,
            sum(p[2] for p in px) / n,
        )
        want = exp.hints["mean_rgb"]
        for i, ch in enumerate("RGB"):
            lo, hi = want[i]
            if not (lo <= means[i] <= hi):
                return False, (
                    f"'{exp.label}' region mean {ch}={means[i]:.0f} outside [{lo},{hi}]"
                )
        return True, f"'{exp.label}' region mean RGB {tuple(round(m) for m in means)} in range"


# =========================================================================== #
# Rosalina overlay driver (closed-loop over the fork's text readout)
# =========================================================================== #

class RosalinaMenuDriver:
    """Drives the Rosalina overlay via InputClient + the text readout.

    The overlay freezes the game AND the GPU, so the NTR stream stalls and
    frame-based verification is impossible while it is open -- the fork's text
    readout (RosalinaReader, UDP command channel) is the only eye. Every press
    here is verified against it: press -> wait_for_change(epoch) -> check the
    cursor/rows, so a dropped input or a menu-layout surprise fails loudly at
    the exact step instead of a blind tap list silently running to the end.

    Closing NEVER presses B at the menu root (a remote B there can hard-freeze
    the console, upstream Luma bug #1852); it sends the fork's MENU_CLOSE
    interface bit, which closes from any depth.
    """

    def __init__(self, input_client, reader,
                 menu_combo: Sequence[str] = ("L", "DDOWN", "SELECT")) -> None:
        self._ic = input_client
        self._reader = reader
        self._combo = tuple(menu_combo)

    # -- primitives --------------------------------------------------------- #
    def read(self, timeout_s: float = 4.0) -> RosalinaScreen:
        """read_once with an OUTER retry deadline on top of the reader's own.

        The command channel can go quiet for >1 s -- during overlay open/close
        transitions, and when a concurrent reader claims the reply tick (both
        live-observed 2026-07-18; an n3ds-mcp session's console_status query
        collided with a walk and starved it past read_once's internal 3-try
        budget). A missing reply inside the deadline means "ask again", not
        "channel dead".
        """
        deadline = time.monotonic() + timeout_s
        while True:
            try:
                return self._reader.read_once()
            except RuntimeError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.25)

    def _read_tolerant(self, deadline: float) -> Optional[RosalinaScreen]:
        """Like :meth:`read`, but returns None at ``deadline`` instead of
        raising (for open/close polls that have their own failure message)."""
        while time.monotonic() < deadline:
            try:
                return self.read(timeout_s=max(0.3, min(1.0, deadline - time.monotonic())))
            except RuntimeError:
                time.sleep(0.2)
        return None

    def _wait_change(self, after_epoch: int, timeout_s: float = 2.0) -> RosalinaScreen:
        """Poll (tolerantly) until the epoch moves past ``after_epoch``.

        Inner reads are scoped to the remaining budget so a stalled channel
        cannot blow the caller's timeout via read()'s own larger default.
        """
        deadline = time.monotonic() + timeout_s
        scr = self.read(timeout_s=timeout_s)
        while scr.epoch == after_epoch and time.monotonic() < deadline:
            time.sleep(0.05)
            scr = self.read(timeout_s=max(0.3, deadline - time.monotonic()))
        return scr

    def press_and_wait(self, buttons: Sequence[str], timeout_s: float = 2.0,
                       after_epoch: Optional[int] = None) -> RosalinaScreen:
        """One verified press: press, then wait for the redraw past the epoch.

        Callers that already hold the current screen pass its epoch via
        ``after_epoch`` to skip the redundant pre-press read (one UDP
        round-trip per press adds up over a 15+-press walk).
        """
        if after_epoch is None:
            after_epoch = self.read().epoch
        self._ic.press(list(buttons), hold_ms=80)
        return self._wait_change(after_epoch, timeout_s=timeout_s)

    def open(self, timeout_s: float = 6.0) -> RosalinaScreen:
        """Open the overlay with the menu chord (idempotent; one re-send).

        NOTE: the chord leaks its DDOWN into the app underneath -- callers must
        re-anchor any app-side navigation after the round-trip.
        """
        scr = self.read()
        if scr.overlay_active:
            return scr
        deadline = time.monotonic() + timeout_s
        resend_at = time.monotonic() + timeout_s / 2.0
        self._ic.press(list(self._combo), hold_ms=150)
        while time.monotonic() < deadline:
            # Poll only up to the resend point so a quiet channel can't hold
            # the loop past it -- otherwise the one re-send below never fires
            # in exactly the lost-packet case it exists for.
            scr = self._read_tolerant(min(resend_at, deadline))
            if scr is not None and scr.overlay_active:
                return scr
            if time.monotonic() >= resend_at:
                # UDP input is fire-and-forget; one lost chord packet must not
                # fail the walk. Re-send once at half the deadline.
                resend_at = deadline
                self._ic.press(list(self._combo), hold_ms=150)
            time.sleep(0.1)
        raise RuntimeError("Rosalina overlay did not open on the menu chord")

    def close(self, timeout_s: float = 3.0) -> None:
        """MENU_CLOSE pulse (fork feature), then wait out the input deadzone."""
        self._ic.set_state(interface=frozenset({"MENU_CLOSE"}))
        time.sleep(0.2)
        self._ic.neutral()
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            scr = self._read_tolerant(deadline)
            if scr is not None and not scr.overlay_active:
                # The resuming app drops inputs for ~1 s -- wait it out so the
                # caller's next tap actually lands (live-measured 2026-07-18).
                time.sleep(POST_MENU_CLOSE_DEADZONE_S)
                return
            time.sleep(0.1)
        raise RuntimeError(
            "Rosalina overlay still active after the close pulse -- is the "
            "n3ds-mcp Luma3DS fork installed? (Stock Luma ignores the bit.)"
        )

    def seek_cursor(self, entry: str, max_steps: int = 20) -> RosalinaScreen:
        """DDOWN until the cursor sits on ``entry`` (prefix match), one verified
        press at a time -- self-healing across menu reorderings.

        ``cursor_text`` keeps the readout's leading ">" marker (the live wire
        format is e.g. '>  Take screenshot'), so strip it before matching.
        """
        scr = self.read()
        for _ in range(max_steps):
            cur = scr.cursor_text
            if cur is not None and cur.lstrip("> ").startswith(entry):
                return scr
            scr = self.press_and_wait(("DDOWN",), after_epoch=scr.epoch)
        raise RuntimeError(f"cursor never reached {entry!r} within {max_steps} DDOWN steps")

    # -- the volume-override walk ------------------------------------------- #
    @staticmethod
    def parse_volume_screen(scr: RosalinaScreen) -> tuple[Optional[bool], Optional[int]]:
        """(enabled, value%) from the volume screen's rows; None when absent."""
        enabled: Optional[bool] = None
        value: Optional[int] = None
        for row in scr.rows:
            m = re.search(r"Current status:\s*(ENABLED|DISABLED)", row.text)
            if m:
                enabled = m.group(1) == "ENABLED"
            m = re.search(r"Value:\s*\[(\d+)%\]", row.text)
            if m:
                value = int(m.group(1))
        return enabled, value

    @staticmethod
    def volume_step_buttons(current: int, target: int) -> list[str]:
        """The minimal D-pad walk from one value to another.

        DLEFT/DRIGHT step +/-10, DUP/DDOWN +/-1 (live-verified). Chooses the
        tens count that minimises total presses (e.g. +15 = 2x DRIGHT + 5x
        DDOWN over 1x DRIGHT + 5x DUP -- both 10-adjacent splits are tried).
        """
        delta = target - current
        best: Optional[list[str]] = None
        for tens in (delta // 10, -((-delta) // 10)):  # floor and ceil of delta/10
            ones = delta - tens * 10
            steps = ["DRIGHT" if tens > 0 else "DLEFT"] * abs(tens)
            steps += ["DUP" if ones > 0 else "DDOWN"] * abs(ones)
            if best is None or len(steps) < len(best):
                best = steps
        return best or []

    def set_volume_override(self, target: int) -> dict:
        """Open the overlay, walk to Control volume, pin the override to
        ``target`` %, apply, verify from the readout, and close.

        Returns {"value", "enabled", "applied"} on success; raises RuntimeError
        with the exact failing step otherwise. Root-menu fact (live-verified):
        the cursor RESETS to the top entry on a fresh open, and seek_cursor
        re-verifies it per press anyway.
        """
        if not (0 <= target <= 100):
            raise ValueError(f"volume override must be 0..100, got {target}")
        try:
            self.open()
            scr = self.seek_cursor(ROSALINA_SYSCONFIG_ENTRY)
            scr = self.press_and_wait(("A",), after_epoch=scr.epoch)
            scr = self.seek_cursor(ROSALINA_VOLUME_ENTRY)
            scr = self.press_and_wait(("A",), after_epoch=scr.epoch)
            enabled, value = self.parse_volume_screen(scr)
            if enabled is None:
                raise RuntimeError(
                    f"did not land on the volume screen (rows: {scr.text!r})")
            if not enabled:
                # While DISABLED there is no "Value:" row (live-observed); Y
                # enables the override and the row appears with its last value.
                scr = self.press_and_wait(("Y",), after_epoch=scr.epoch)
                enabled, value = self.parse_volume_screen(scr)
                if not enabled or value is None:
                    raise RuntimeError("Y did not ENABLE the volume override")
            if value is None:
                raise RuntimeError(
                    f"override ENABLED but no Value row (rows: {scr.text!r})")
            for b in self.volume_step_buttons(value, target):
                scr = self.press_and_wait((b,), after_epoch=scr.epoch)
            _, value = self.parse_volume_screen(scr)
            if value != target:
                raise RuntimeError(
                    f"stepped to {value}%, wanted {target}% -- readout disagrees")
            scr = self.press_and_wait(("A",), after_epoch=scr.epoch)  # Apply -> "Success!"
            _, value = self.parse_volume_screen(scr)
            if "Success!" not in scr.text or value != target:
                raise RuntimeError(
                    f"apply not confirmed (value {value}%, text {scr.text!r})")
        except BaseException:
            # Never leave the console frozen -- but a secondary close failure
            # must not mask the original walk error.
            try:
                self.close()
            except Exception:
                pass
            raise
        self.close()
        return {"value": value, "enabled": True, "applied": True}


# =========================================================================== #
# Plaza navigation path (live-verified on the console, 2026-07-18)
# =========================================================================== #

# Music-player track order in the plaza: Entrance, Main Theme 1 (= battery A),
# Main Theme 2, ... The value is the number of DDOWN presses from the top of
# the list to that track. Battery B displays as "Find Mii - Dark Room", well
# below the Main Themes -- its DDOWN count has not been walked yet; fill it in
# (same screenshot-verified way) the next time battery B is captured.
PLAZA_TRACKS: dict[str, int] = {
    "Entrance": 0,
    "Main Theme 1": 1,   # = capture-cartridge battery A (BGM_MAIN_Mii_Only_One)
    "Main Theme 2": 2,
}

# The HOME-menu -> plaza-tile prefix, live-verified 2026-07-18. The HOME menu
# layout TODAY (it shifts when software is added/moved): leftmost tile =
# "NTR CFW", then the Homebrew Utils folder, a blank tile, then StreetPass Mii
# Plaza as the 4th tile. The prefix is layout-robust up to the DRIGHT count:
# saturate DLEFT (12 exceeds any plausible row length; extra presses no-op at
# the edge), then 3x DRIGHT -- and the checkpoint verifies the TOP screen shows
# the plaza banner (selecting a tile names it there) BEFORE the A that
# launches. Start-state contract (v0): HOME menu with NO suspended software --
# a fresh launch shows splash -> "Please wait" -> the "Careful!" modal ("Play"
# preselected); resuming a suspended plaza skips all three, which this path
# does not model.
DEFAULT_HOME_TO_PLAZA_TAPS: tuple[Tap, ...] = (
    Tap(("HOME",), wait_ms=1500),
) + tuple(
    Tap(("DLEFT",), wait_ms=250) for _ in range(12)
) + tuple(
    Tap(("DRIGHT",), wait_ms=250) for _ in range(3)
)

# Live-measured navigation timings (2026-07-18) -- named next to the other
# calibrated waits so a future retune finds them all in one place.
PLAZA_LAUNCH_WAIT_MS = 12000    # splash + "Please wait" took ~11 s live
MODAL_CLEAR_SETTLE_MS = 4000    # shakedown take-4: a Y sent ~2.4 s after the
                                # last card-dismiss A was SWALLOWED by the
                                # dismissal animation -- this settle is
                                # load-bearing, not cosmetic

# The Music Player tile on the plaza FEATURES grid (4x3, bottom screen):
# row 3 col 1 (bottom-left, the music note) -> touch center. The features grid
# only exists after Y ("Switch Icons") toggles away from the StreetPass GAMES
# grid -- the games grid never contains the Music Player. Touching a tile
# selects + names it on the top screen WITHOUT launching (the reliable
# identify trick); the following A opens it.
MUSIC_PLAYER_TOUCH_XY: tuple[int, int] = (52, 172)


def plaza_music_player_path(
    ddown_count: int,
    *,
    launch_taps: Sequence[Tap] = DEFAULT_HOME_TO_PLAZA_TAPS,
    modal_clear_a_presses: int = 2,
    music_player_touch: tuple[int, int] = MUSIC_PLAYER_TOUCH_XY,
) -> list[NavStep]:
    """Build the data-driven navigation path to a plaza music-player track.

    Live-verified flow (2026-07-18): HOME -> select the plaza tile (banner
    checkpoint) -> A launches (splash + "Please wait", ~11 s) -> A through the
    "Careful!" modal ("Play" preselected) -> A through the announcement cards
    ("Someone's here", "New stock", ... -- ONE A each; the count varies with
    pending announcements, 2 on the verified run; an extra A opens your own
    Mii's menu, B backs out of it) -> Y switches the bottom grid to FEATURES ->
    touch selects Music Player (top screen names it) -> A opens the track list
    (cursor on Entrance) -> DDOWN * n -> A plays ("A: Stop" appears).

    Every step is a checkpoint so a wrong count/layout fails loudly at its step
    rather than silently mis-navigating. NOTE: the default AcceptingVerifier
    only proves the stream produced a frame per step -- true screen identity
    (e.g. catching a third announcement card) needs a RegionMean/template
    verifier over the ExpectScreen hints.
    """
    steps: list[NavStep] = []
    steps.append(NavStep(
        "select-plaza-tile",
        tuple(launch_taps),
        ExpectScreen("plaza-tile-selected", "top",
                     hints={"note": "top screen shows the StreetPass Mii Plaza banner"}),
        settle_ms=500,
    ))
    steps.append(NavStep(
        "launch-plaza",
        (Tap(("A",), wait_ms=PLAZA_LAUNCH_WAIT_MS),),
        ExpectScreen("careful-modal", "top",
                     hints={"note": "'Careful!' modal, 'Play' preselected"}),
        settle_ms=1000,
    ))
    steps.append(NavStep(
        "enter-plaza",
        (Tap(("A",), wait_ms=5000),),
        ExpectScreen("plaza-cards", "top",
                     hints={"note": "walkable plaza loads; announcement cards may stack"}),
        settle_ms=1000,
    ))
    steps.append(NavStep(
        "clear-modals",
        tuple(Tap(("A",), wait_ms=2500) for _ in range(modal_clear_a_presses)),
        ExpectScreen("plaza-main", "top",
                     hints={"note": "walkable plaza, 'Plaza' label, no card overlay"}),
        settle_ms=MODAL_CLEAR_SETTLE_MS,
    ))
    steps.append(NavStep(
        "switch-icons",
        (Tap(("Y",), hold_ms=120, wait_ms=1200),),
        ExpectScreen("features-grid", "bottom",
                     hints={"note": "features grid (music note bottom-left)"}),
        settle_ms=1000,
    ))
    steps.append(NavStep(
        "select-music-player",
        (Tap((), touch=music_player_touch, hold_ms=120, wait_ms=1000),),
        ExpectScreen("music-player-named", "top",
                     hints={"note": "top screen names 'Music Player'"}),
        settle_ms=500,
    ))
    steps.append(NavStep(
        "open-music-player",
        (Tap(("A",), wait_ms=2500),),
        ExpectScreen("track-list", "top",
                     hints={"note": "track list visible, cursor on Entrance"}),
        settle_ms=800,
    ))
    if ddown_count > 0:
        steps.append(NavStep(
            "select-track",
            tuple(Tap(("DDOWN",), wait_ms=300) for _ in range(ddown_count)),
            ExpectScreen("track-highlighted", "top",
                         hints={"note": f"track {ddown_count} down highlighted"}),
            settle_ms=400,
        ))
    steps.append(NavStep(
        "play",
        (Tap(("A",), wait_ms=500),),
        ExpectScreen("playing", "bottom",
                     hints={"note": "bottom bar shows 'A: Stop' (playing state)"}),
        settle_ms=600,
    ))
    return steps


def plaza_path_for_track(track_name: str, **kwargs) -> list[NavStep]:
    """Navigation path for a named track (see :data:`PLAZA_TRACKS`)."""
    if track_name not in PLAZA_TRACKS:
        raise KeyError(
            f"unknown plaza track {track_name!r}; known: {sorted(PLAZA_TRACKS)}"
        )
    return plaza_music_player_path(PLAZA_TRACKS[track_name], **kwargs)


# The A tap that stops playback (memory: "A plays, A stops").
STOP_PLAYBACK_TAP = Tap(("A",), wait_ms=300)


# =========================================================================== #
# Verdict runner (shells out to tools/console-tolerance)
# =========================================================================== #

VerdictRunner = Callable[[list[str]], tuple[int, str, str]]


def _subprocess_verdict_runner(argv: list[str]) -> tuple[int, str, str]:
    """Run console_tolerance.py and return (exit, stdout, stderr)."""
    proc = subprocess.run(argv, capture_output=True, text=True, timeout=600)
    return proc.returncode, proc.stdout, proc.stderr


# =========================================================================== #
# The pipeline
# =========================================================================== #

class ConsoleCaptureSession:
    """Orchestrates the capture pipeline over reusable, injectable dependencies.

    Every collaborator can be injected for offline testing against the n3ds-mcp
    simulators: an ``InputClient`` (real, pointed at SimInputSink), an
    ``NTRClient`` (real, pointed at SimNTR), the ftp callables (fake ftp store),
    the recorder (fake ffmpeg) and the verdict runner (canned exit codes). With
    nothing injected it lazily builds real clients from the config.
    """

    def __init__(
        self,
        cfg: CaptureConfig,
        *,
        input_client: Optional[InputClient] = None,
        ntr_client: Optional[NTRClient] = None,
        ftp_put: Optional[Callable] = None,
        ftp_get: Optional[Callable] = None,
        ftp_list: Optional[Callable] = None,
        recorder: Optional[Recorder] = None,
        verifier: Optional[ScreenVerifier] = None,
        verdict_runner: Optional[VerdictRunner] = None,
        rosalina_driver: Optional[RosalinaMenuDriver] = None,
    ) -> None:
        self.cfg = cfg
        self._input = input_client
        self._ntr = ntr_client
        self._ntr_started = ntr_client is not None
        self._ftp_put = ftp_put or devtools.ftp_put
        self._ftp_get = ftp_get or devtools.ftp_get
        self._ftp_list = ftp_list or devtools.ftp_list
        self._recorder = recorder or FfmpegRecorder()
        self._verifier = verifier or AcceptingVerifier()
        self._verdict_runner = verdict_runner or _subprocess_verdict_runner
        self._rosalina = rosalina_driver

    # -- lazy real clients -------------------------------------------------- #
    def input_client(self) -> InputClient:
        if self._input is None:
            ip = self.cfg.n3ds.require_ip()
            self._input = InputClient(ip, self.cfg.n3ds.input_port)
        return self._input

    def ntr_client(self) -> NTRClient:
        if self._ntr is None:
            ip = self.cfg.n3ds.require_ip()
            self._ntr = NTRClient(
                ip,
                control_port=self.cfg.n3ds.control_port,
                video_port=self.cfg.n3ds.video_port,
                quality=self.cfg.n3ds.quality,
                priority_screen=self.cfg.n3ds.priority_screen,
            )
        if not self._ntr_started:
            self._ntr.start()
            self._ntr_started = True
        return self._ntr

    def rosalina_driver(self) -> RosalinaMenuDriver:
        if self._rosalina is None:
            ip = self.cfg.n3ds.require_ip()
            self._rosalina = RosalinaMenuDriver(
                self.input_client(),
                RosalinaReader(ip, self.cfg.n3ds.cmd_port),
                menu_combo=sorted(self.cfg.n3ds.menu_combo),
            )
        return self._rosalina

    def close(self) -> None:
        if self._ntr is not None and self._ntr_started:
            try:
                self._ntr.stop()
            except Exception:
                pass
        if self._input is not None:
            try:
                self._input.close()
            except Exception:
                pass

    def __enter__(self) -> "ConsoleCaptureSession":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    # -- helpers ------------------------------------------------------------ #
    def _wait_frame(self, screen: str, timeout: float = 6.0):
        """Poll the NTR client until a fresh frame for ``screen`` decodes."""
        ntr = self.ntr_client()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            got = ntr.get_frame(screen)
            if got is not None:
                return got[0]
            time.sleep(0.05)
        return None

    def _run_taps(self, taps: Sequence[Tap]) -> None:
        ic = self.input_client()
        for tap in taps:
            if tap.touch is not None:
                ic.touch(tap.touch[0], tap.touch[1], hold_ms=tap.hold_ms)
            if tap.buttons:
                ic.press(list(tap.buttons), hold_ms=tap.hold_ms)
            if tap.wait_ms > 0:
                time.sleep(tap.wait_ms / 1000.0)

    # -- stage 1: preflight ------------------------------------------------- #
    def preflight(self, *, reapply_volume: bool = True) -> StageResult:
        """File service reachable + NTR stream healthy, and (optionally)
        re-apply the Rosalina volume-override pin."""
        data: dict = {}

        # File service reachable: a directory listing round-trips. (Default =
        # the Rosalina file service, port 4952, on at boot in dev mode.)
        try:
            entries = self._ftp_list(self.cfg.n3ds.require_ip(), "/", self.cfg.n3ds.ftp_port)
            data["ftp_entries"] = len(entries)
        except Exception as exc:
            return StageResult(
                "preflight", False,
                f"file service unreachable (port {self.cfg.n3ds.ftp_port}): {exc}", data)

        # NTR stream healthy: a frame arrives and status reports connected.
        try:
            frame = self._wait_frame("top", timeout=6.0)
            st = self.ntr_client().status()
            data["ntr"] = st
            if frame is None or not st.get("connected"):
                return StageResult(
                    "preflight", False,
                    f"NTR stream not healthy (connected={st.get('connected')}, "
                    f"frame={'yes' if frame is not None else 'no'})", data)
        except Exception as exc:
            return StageResult("preflight", False, f"NTR stream error: {exc}", data)

        # Re-apply the Rosalina volume-override pin (readout-verified).
        vol = self.reapply_rosalina_volume() if reapply_volume else None
        if vol is not None:
            data["volume"] = vol.data
            if not vol.ok:
                return StageResult("preflight", False, f"volume re-apply failed: {vol.message}", data)

        return StageResult("preflight", True,
                           f"file service + NTR healthy (fps {data['ntr'].get('fps', 0):.1f})", data)

    def reapply_rosalina_volume(self) -> StageResult:
        """Re-apply the Rosalina volume-override pin (readout-verified).

        The camera-shutter path can silently reset the override, so the capture
        preflight re-applies it every run. The walk is closed-loop over the
        Rosalina text readout (see :class:`RosalinaMenuDriver`): every press is
        confirmed from the cursor text and the applied value is read back from
        the "Value: [NN%]" row, so a drifted menu or a dropped input FAILS the
        preflight instead of silently mis-pinning the level. Run this while the
        console sits on the HOME menu: the chord's DDOWN leak is harmless there
        and navigate()'s DLEFT saturation re-anchors afterwards.
        """
        if self.cfg.rosalina_volume is None:
            return StageResult(
                "volume", True,
                "SKIPPED: no Rosalina volume override configured "
                "(CaptureConfig.rosalina_volume is None); the analog slider "
                "position is authoritative for this run.",
                {"applied": False, "volume": None, "skipped": True},
            )
        try:
            result = self.rosalina_driver().set_volume_override(self.cfg.rosalina_volume)
        except Exception as exc:
            return StageResult(
                "volume", False,
                f"volume-override walk failed: {exc}",
                {"applied": False, "volume": self.cfg.rosalina_volume, "todo": False},
            )
        return StageResult(
            "volume", True,
            f"Rosalina volume override pinned at {result['value']}% (readout-verified)",
            {**result, "volume": result["value"], "todo": False},
        )

    # -- stage 2: deploy ---------------------------------------------------- #
    def deploy(self, local_bcsar: str | Path, *, remote: Optional[str] = None) -> StageResult:
        """Verify-first cartridge deploy: read the remote back and hash-compare;
        push only when the bytes differ.

        The boot-time Rosalina file service (port 4952) is WRITE-CONFINED to
        /luma/staging/ (a fork safety design, live-observed 2026-07-18), so an
        already-installed cartridge can always be VERIFIED at boot, but pushing
        a NEW cartridge to the Luma titles path needs legacy ftpd (launch it
        console-side and set N3DS_FTP_PORT=5000).
        """
        local_bcsar = Path(local_bcsar)
        if not local_bcsar.is_file():
            return StageResult("deploy", False, f"local .bcsar not found: {local_bcsar}", {})
        remote = remote or self.cfg.luma_bcsar_path
        ip = self.cfg.n3ds.require_ip()
        port = self.cfg.n3ds.ftp_port

        local_hash = hashlib.sha256(local_bcsar.read_bytes()).hexdigest()
        readback = self.cfg.captures_dir / (local_bcsar.stem + ".readback.bcsar")
        readback.parent.mkdir(parents=True, exist_ok=True)

        def remote_sha256() -> str:
            self._ftp_get(ip, remote, str(readback), port)
            return hashlib.sha256(readback.read_bytes()).hexdigest()

        # Read-back first: if the remote already carries these exact bytes the
        # push is a no-op -- and it works over the write-confined boot service.
        preread_error: Optional[str] = None
        try:
            if remote_sha256() == local_hash:
                return StageResult(
                    "deploy", True,
                    f"already deployed + verified (sha256 {local_hash[:12]}, no push needed)",
                    {"remote": remote, "local_sha256": local_hash,
                     "remote_sha256": local_hash, "readback": str(readback),
                     "pushed": False})
        except Exception as exc:
            # A missing remote (fresh install) falls through to the push; keep
            # the error so a transport failure stays diagnosable if the push
            # then fails too.
            preread_error = str(exc)

        try:
            self._ftp_put(ip, str(local_bcsar), remote, port)
        except Exception as exc:
            hint = ""
            # The boot-time file service (its default port) is ALWAYS
            # write-confined; don't rely only on the fork's error spelling.
            if port == DEFAULT_FTP_PORT or "staging" in str(exc):
                hint = (" -- the boot-time file service only writes under "
                        "/luma/staging/; launch ftpd on the console and set "
                        "N3DS_FTP_PORT=5000 to push a new cartridge")
            return StageResult("deploy", False, f"ftp_put failed: {exc}{hint}",
                               {"remote": remote, "local_sha256": local_hash,
                                "preread_error": preread_error})

        try:
            remote_hash = remote_sha256()
        except Exception as exc:
            return StageResult("deploy", False, f"ftp_get readback failed: {exc}",
                               {"remote": remote, "local_sha256": local_hash})

        data = {"remote": remote, "local_sha256": local_hash,
                "remote_sha256": remote_hash, "readback": str(readback),
                "pushed": True}
        if remote_hash != local_hash:
            return StageResult("deploy", False,
                               f"hash mismatch after push (local {local_hash[:12]} != "
                               f"remote {remote_hash[:12]}) -- the push did not land intact", data)
        return StageResult("deploy", True,
                           f"cartridge pushed + verified (sha256 {local_hash[:12]})", data)

    # -- stage 3: navigate -------------------------------------------------- #
    def navigate(self, path: Sequence[NavStep], *,
                 verifier: Optional[ScreenVerifier] = None) -> StageResult:
        """Drive a data-driven navigation path, verifying a frame at each step.

        Aborts at the first checkpoint whose verifier rejects the frame, reporting
        the failed step name + index so a wrong path is diagnosable without a
        human watching the screen.
        """
        verifier = verifier or self._verifier
        checkpoints: list[dict] = []
        for i, step in enumerate(path):
            self._run_taps(step.taps)
            if step.settle_ms > 0:
                time.sleep(step.settle_ms / 1000.0)
            screen = step.expect.screen if step.expect else "top"
            frame = self._wait_frame(screen, timeout=6.0)
            ok, note = verifier.verify(step, frame)
            checkpoints.append({"step": step.name, "index": i, "ok": ok, "note": note})
            if not ok:
                return StageResult(
                    "navigate", False,
                    f"checkpoint '{step.name}' (#{i}) failed: {note}",
                    {"checkpoints": checkpoints, "failed_step": step.name, "failed_index": i},
                )
        return StageResult("navigate", True,
                           f"reached target through {len(path)} checkpoints",
                           {"checkpoints": checkpoints})

    def stop_playback(self) -> None:
        """Send the stop-playback tap (A) after a recording."""
        self._run_taps((STOP_PLAYBACK_TAP,))

    # -- stage 4: record ---------------------------------------------------- #
    def record(self, out_name: str, *, passes: int = 2,
               seconds_per_pass: Optional[float] = None,
               assert_levels: bool = True) -> StageResult:
        """Record ``passes`` worth of seconds to ``captures_dir/out_name`` and run
        the level/noise assertion on the result."""
        spp = self.cfg.seconds_per_pass if seconds_per_pass is None else seconds_per_pass
        seconds = record_seconds(passes, spp, self.cfg.lead_in_s)
        self.cfg.captures_dir.mkdir(parents=True, exist_ok=True)
        out_path = self.cfg.captures_dir / out_name
        try:
            self._recorder.record(self.cfg, seconds, out_path)
        except Exception as exc:
            return StageResult("record", False, f"recording failed: {exc}",
                               {"seconds": seconds, "passes": passes, "out": str(out_path)})
        if not out_path.is_file():
            return StageResult("record", False, "recorder produced no file",
                               {"seconds": seconds, "out": str(out_path)})

        base = {"seconds": seconds, "passes": passes, "out": str(out_path)}
        if not assert_levels:
            return StageResult("record", True, f"recorded {seconds:.1f}s -> {out_path.name}", base)
        try:
            report = analyze_levels(out_path)
        except Exception as exc:
            return StageResult("record", False, f"level analysis failed: {exc}", base)
        verdict = assert_capture_levels(report, self.cfg)
        base.update(verdict.data)
        return StageResult("record", verdict.ok,
                           f"recorded {seconds:.1f}s -> {out_path.name}; {verdict.message}", base)

    # -- stage 5: verdict --------------------------------------------------- #
    def verdict(self, capture_wav: str | Path, render_wav: str | Path) -> StageResult:
        """Hand the capture + a caesar-play render to console-tolerance and map
        its exit code (0 PASS, 1 out-of-tolerance, 2 harness error)."""
        tol = self.cfg.console_tolerance_py
        if not Path(tol).is_file():
            return StageResult("verdict", False, f"console_tolerance.py not found: {tol}", {})
        argv = [self.cfg.python_exe, str(tol), str(capture_wav), str(render_wav)]
        try:
            code, out, err = self._verdict_runner(argv)
        except Exception as exc:
            return StageResult("verdict", False, f"could not run console-tolerance: {exc}", {})
        data = {"exit": code, "stdout_tail": out[-2000:], "stderr_tail": err[-1000:]}
        if code == 0:
            return StageResult("verdict", True, "PASS: within tolerance (reverb tail excepted)", data)
        if code == 1:
            return StageResult("verdict", False, "FAIL: out of tolerance", data)
        return StageResult("verdict", False,
                           f"HARNESS ERROR from console-tolerance (exit {code}) -- NOT a pass", data)

    # -- stage 6: perturbation A/B ----------------------------------------- #
    def perturbation_ab(self, *, passes: int = 1,
                        seconds_per_pass: Optional[float] = None,
                        on_name: str = "ntr_on.wav",
                        off_name: str = "ntr_off.wav",
                        restart_ntr: bool = True) -> StageResult:
        """Record the same pass twice -- NTR streaming ON then OFF -- and report
        whether the audio moved beyond noise. A DAC-perturbation check: if the two
        recordings' per-channel peak/RMS agree within ``ab_level_tol_db``, the NTR
        stream does not disturb the analog output.

        NOTE: this is a level-domain proxy. A finer spectral A/B is the
        console-tolerance-grade follow-up; this cheap check is the go/no-go gate.
        """
        # Pass 1: NTR streaming ON (ensure it is up).
        self.ntr_client()
        on = self.record(on_name, passes=passes, seconds_per_pass=seconds_per_pass)
        if not on.ok:
            return StageResult("perturb_ab", False, f"NTR-ON record failed: {on.message}", {"on": on.data})

        # Stop the NTR stream, then Pass 2 with streaming OFF.
        if self._ntr is not None and self._ntr_started:
            self._ntr.stop()
            self._ntr_started = False
        off = self.record(off_name, passes=passes, seconds_per_pass=seconds_per_pass)

        if restart_ntr:
            try:
                self.ntr_client()  # re-arm for any following stages
            except Exception:
                pass
        if not off.ok:
            return StageResult("perturb_ab", False, f"NTR-OFF record failed: {off.message}",
                               {"on": on.data, "off": off.data})

        # Compare per-channel peak + RMS.
        r_on = analyze_levels(on.data["out"])
        r_off = analyze_levels(off.data["out"])
        nch = min(r_on.channels, r_off.channels)
        deltas = []
        worst = 0.0
        for ci in range(nch):
            dpk = abs(r_on.peak_dbfs[ci] - r_off.peak_dbfs[ci])
            drms = abs(r_on.rms_dbfs[ci] - r_off.rms_dbfs[ci])
            deltas.append({"channel": ci, "d_peak_db": dpk, "d_rms_db": drms})
            worst = max(worst, dpk, drms)
        data = {"deltas": deltas, "worst_db": worst, "tol_db": self.cfg.ab_level_tol_db,
                "on": on.data["out"], "off": off.data["out"]}
        if worst > self.cfg.ab_level_tol_db:
            return StageResult("perturb_ab", False,
                               f"NTR streaming PERTURBS the audio (worst {worst:.2f} dB > "
                               f"{self.cfg.ab_level_tol_db} dB tolerance)", data)
        return StageResult("perturb_ab", True,
                           f"NTR streaming does not perturb the audio (worst {worst:.2f} dB <= "
                           f"{self.cfg.ab_level_tol_db} dB)", data)


# =========================================================================== #
# CLI (thin; the importable API above is the real deliverable)
# =========================================================================== #

def _build_cli():
    import argparse

    ap = argparse.ArgumentParser(
        prog="console-capture",
        description="Hands-free New-3DS audio capture pipeline (reuses n3ds-mcp).",
    )
    ap.add_argument("--ip", help="Console IP (overrides N3DS_IP).")
    ap.add_argument("--captures-dir", help="Host directory for captured WAVs.")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("preflight", help="File-service + NTR health and volume re-apply.")

    d = sub.add_parser("deploy", help="Push a cartridge .bcsar and hash-verify it.")
    d.add_argument("bcsar")

    n = sub.add_parser("navigate", help="Navigate to a plaza track (screenshot-verified).")
    n.add_argument("--track", default="Main Theme 1", choices=sorted(PLAZA_TRACKS))

    r = sub.add_parser("record", help="Record N passes and level-check.")
    r.add_argument("out")
    r.add_argument("--passes", type=int, default=2)
    r.add_argument("--seconds-per-pass", type=float, default=None)
    r.add_argument("--battery", choices=sorted(SECONDS_PER_PASS_V2),
                   help="Battery-v2 track letter; sets the pass length from "
                        "SECONDS_PER_PASS_V2 unless --seconds-per-pass is given.")

    v = sub.add_parser("verdict", help="console-tolerance verdict of a capture vs a render.")
    v.add_argument("capture")
    v.add_argument("render")

    ab = sub.add_parser("ab", help="NTR streaming on/off perturbation A/B.")
    ab.add_argument("--passes", type=int, default=1)
    return ap


def main(argv: Optional[list[str]] = None) -> int:
    ap = _build_cli()
    args = ap.parse_args(argv)

    cfg = CaptureConfig()
    if args.ip:
        cfg.n3ds.ip = args.ip
    if args.captures_dir:
        cfg.captures_dir = Path(args.captures_dir)

    with ConsoleCaptureSession(cfg) as sess:
        if args.cmd == "preflight":
            res = sess.preflight()
        elif args.cmd == "deploy":
            res = sess.deploy(args.bcsar)
        elif args.cmd == "navigate":
            res = sess.navigate(plaza_path_for_track(args.track))
        elif args.cmd == "record":
            spp = args.seconds_per_pass
            if spp is None and args.battery:
                spp = SECONDS_PER_PASS_V2[args.battery]
            res = sess.record(args.out, passes=args.passes, seconds_per_pass=spp)
        elif args.cmd == "verdict":
            res = sess.verdict(args.capture, args.render)
        elif args.cmd == "ab":
            res = sess.perturbation_ab(passes=args.passes)
        else:  # pragma: no cover - argparse enforces choices
            ap.error(f"unknown command {args.cmd}")
            return 2
        print(res.summary())
        return 0 if res.ok else 1


if __name__ == "__main__":
    sys.exit(main())
