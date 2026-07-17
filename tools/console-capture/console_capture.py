#!/usr/bin/env python3
"""console_capture.py -- the hands-free New-3DS audio capture harness.

Turns the capture loop that has cost a human session each time (build -> SD ->
navigate -> record -> hand over WAVs) into a scriptable pipeline. Every stage is
independently callable and returns a :class:`StageResult` with an explicit
success/failure and a machine-readable ``data`` payload:

  1. preflight   -- ftpd reachable + NTR stream healthy, and re-apply the
                    Rosalina volume-override pin (parameterised; see the TODO).
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
InputClient (input_redirect), NTRClient (ntr), ftp_put/ftp_get/ftp_list
(devtools), and Config (config). The only in-process dependency beyond n3ds_mcp
is the standard library; numpy is used ONLY as an optional accelerator for the
level analysis and is never required (the console-tolerance verdict, which does
need numpy, runs as a subprocess). Rig constants and the plaza navigation path
are transcribed from the ``capture-rig-calibration`` memory (2026-07-15).

OFFLINE-safe: nothing here sends to a live console until a stage method is
called with real clients. Every stage takes injectable dependencies so the whole
pipeline is exercisable against the n3ds-mcp simulators with no hardware.
"""

from __future__ import annotations

import hashlib
import math
import os
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
from n3ds_mcp.config import Config, config_from_env  # noqa: E402
from n3ds_mcp.input_redirect import InputClient  # noqa: E402
from n3ds_mcp.ntr import NTRClient  # noqa: E402


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

# One battery pass, seconds. Battery A ("Main Theme 1") loops ~23.5 s (roadmap:
# "passes exactly 23.50 s apart"); battery B ~23 s.
SECONDS_PER_PASS = 23.5

# --- Rosalina volume override (rig v2; NOT yet console-measured) ------------ #
# TODO(main session): the digitally-exact volume override that replaces the
# frozen analog slider. Two things must be filled in from the live console:
#   * ROSALINA_VOLUME_OVERRIDE -- the chosen 0..100 (%) value that lands peaks in
#     the target window; becomes a rig constant in the calibration memory once
#     measured. None here means "not yet measured" and the preflight step SKIPS
#     the re-apply with a clear notice instead of failing.
#   * ROSALINA_VOLUME_TAPS -- the InputRedirection tap sequence that opens the
#     Rosalina menu (L+DDOWN+SELECT) and walks to the volume-override slider,
#     setting it to ROSALINA_VOLUME_OVERRIDE. Left empty until the menu path is
#     captured on hardware; encoded as a data-driven Tap list (below) so it drops
#     straight into reapply_rosalina_volume() with no code change.
# The camera-shutter path can silently reset the override, which is exactly why
# the capture preflight re-applies it every run.
ROSALINA_VOLUME_OVERRIDE: Optional[int] = None
# Placeholder for the menu-open chord; the walk-to-slider steps are appended by
# the main session once observed on the NTR stream.
ROSALINA_MENU_CHORD: tuple[str, ...] = ("L", "DDOWN", "SELECT")


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
    """One button tap in a navigation step. ``buttons`` may include HID buttons
    (A, DLEFT, DDOWN, ...) and/or interface strings (HOME, POWER) -- InputClient
    routes each to the correct wire field. ``wait_ms`` is the settle delay AFTER
    the release, before the next tap (menu animations)."""

    buttons: tuple[str, ...]
    hold_ms: int = 80
    wait_ms: int = 350


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

    The n3ds ``Config`` carries the console IP + ports (input UDP 4950, NTR
    control TCP 8000, video UDP 8001, ftpd 5000). The capture-specific rig
    constants default from the calibration memory and are all overridable.
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

    # Rosalina volume override (see the module TODO)
    rosalina_volume: Optional[int] = ROSALINA_VOLUME_OVERRIDE
    rosalina_volume_taps: tuple[Tap, ...] = ()

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
# Plaza navigation path (transcribed from the calibration memory, 2026-07-15)
# =========================================================================== #

# Music-player track order in the plaza (memory): Entrance, Main Theme 1
# (= battery A), Main Theme 2, then the den tracks. The value is the number of
# DDOWN presses from the top of the list to that track.
PLAZA_TRACKS: dict[str, int] = {
    "Entrance": 0,
    "Main Theme 1": 1,   # = capture-cartridge battery A (BGM_MAIN_Mii_Only_One)
    "Main Theme 2": 2,
}

# TODO(main session): the HOME-menu -> launch-plaza tap prefix. The exact icon
# position (how many DRIGHT from the HOME cursor, then A) is console-specific and
# was never written down; observe it on the NTR stream and fill this in. Until
# then the default is HOME (open menu) + a single A, and navigate() will still
# report exactly which checkpoint first fails so the gap is obvious on a dry run.
DEFAULT_HOME_TO_PLAZA_TAPS: tuple[Tap, ...] = (
    Tap(("HOME",), wait_ms=800),
    Tap(("A",), wait_ms=3000),  # placeholder: select+launch the plaza icon
)


def plaza_music_player_path(
    ddown_count: int,
    *,
    launch_taps: Sequence[Tap] = DEFAULT_HOME_TO_PLAZA_TAPS,
    modal_clear_a_presses: int = 3,
    dleft_to_music_player: int = 6,
) -> list[NavStep]:
    """Build the data-driven navigation path to a plaza music-player track.

    From the memory: HOME -> StreetPass Mii Plaza -> A through announcement modals
    (D-pad is dead there; the cards need A) -> main plaza -> DLEFT walks the
    selector left to Music Player (far left) -> A -> track list -> DDOWN * n -> A
    plays. ``dleft_to_music_player`` and ``modal_clear_a_presses`` are counts to
    confirm on hardware; each is a checkpoint so a wrong count fails loudly at its
    step rather than silently mis-navigating.
    """
    steps: list[NavStep] = []
    steps.append(NavStep(
        "launch-plaza",
        tuple(launch_taps),
        ExpectScreen("plaza-intro", "top", hints={"note": "plaza title/intro screen"}),
        settle_ms=1500,
    ))
    steps.append(NavStep(
        "clear-modals",
        tuple(Tap(("A",), wait_ms=700) for _ in range(modal_clear_a_presses)),
        ExpectScreen("plaza-main", "top", hints={"note": "main plaza, selector visible"}),
        settle_ms=800,
    ))
    steps.append(NavStep(
        "to-music-player",
        tuple(Tap(("DLEFT",), wait_ms=350) for _ in range(dleft_to_music_player)),
        ExpectScreen("music-player-selected", "bottom",
                     hints={"note": "Music Player icon (black note) highlighted, far left"}),
        settle_ms=500,
    ))
    steps.append(NavStep(
        "open-music-player",
        (Tap(("A",), wait_ms=800),),
        ExpectScreen("track-list", "bottom", hints={"note": "track list visible"}),
        settle_ms=800,
    ))
    if ddown_count > 0:
        steps.append(NavStep(
            "select-track",
            tuple(Tap(("DDOWN",), wait_ms=300) for _ in range(ddown_count)),
            ExpectScreen("track-highlighted", "bottom",
                         hints={"note": f"track {ddown_count} down highlighted"}),
            settle_ms=400,
        ))
    steps.append(NavStep(
        "play",
        (Tap(("A",), wait_ms=500),),
        ExpectScreen("playing", "bottom", hints={"note": "playback started"}),
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
            ic.press(list(tap.buttons), hold_ms=tap.hold_ms)
            if tap.wait_ms > 0:
                time.sleep(tap.wait_ms / 1000.0)

    # -- stage 1: preflight ------------------------------------------------- #
    def preflight(self, *, reapply_volume: bool = True) -> StageResult:
        """ftpd reachable + NTR stream healthy, and (optionally) re-apply the
        Rosalina volume-override pin."""
        data: dict = {}

        # ftpd reachable: a directory listing round-trips.
        try:
            entries = self._ftp_list(self.cfg.n3ds.require_ip(), "/", self.cfg.n3ds.ftp_port)
            data["ftp_entries"] = len(entries)
        except Exception as exc:
            return StageResult("preflight", False, f"ftpd unreachable: {exc}", data)

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

        # Re-apply the Rosalina volume-override pin (parameterised; see TODO).
        vol = self.reapply_rosalina_volume() if reapply_volume else None
        if vol is not None:
            data["volume"] = vol.data
            if not vol.ok:
                return StageResult("preflight", False, f"volume re-apply failed: {vol.message}", data)

        return StageResult("preflight", True,
                           f"ftpd + NTR healthy (fps {data['ntr'].get('fps', 0):.1f})", data)

    def reapply_rosalina_volume(self) -> StageResult:
        """Re-apply the Rosalina volume-override pin.

        The camera-shutter path can silently reset the override, so the capture
        preflight re-applies it every run. This is the documented, parameterised
        hook: it runs ``cfg.rosalina_volume_taps`` (the live menu sequence the
        main session captures) when they are defined, and otherwise SKIPS with a
        clear notice -- it never fails preflight just because the sequence hasn't
        been recorded yet. The chosen value (``cfg.rosalina_volume``) becomes a
        rig constant in the calibration memory once measured.
        """
        if not self.cfg.rosalina_volume_taps or self.cfg.rosalina_volume is None:
            return StageResult(
                "volume", True,
                "SKIPPED (TODO): Rosalina volume-override sequence not yet recorded on "
                "hardware; set CaptureConfig.rosalina_volume + rosalina_volume_taps "
                "(menu chord " + "+".join(ROSALINA_MENU_CHORD) + ") from the live console.",
                {"applied": False, "volume": self.cfg.rosalina_volume, "todo": True},
            )
        # TODO(main session): verify the slider landed on the target value via an
        # NTR-frame region read before trusting it; for now we send the sequence.
        self._run_taps(self.cfg.rosalina_volume_taps)
        return StageResult(
            "volume", True,
            f"applied Rosalina volume override = {self.cfg.rosalina_volume}",
            {"applied": True, "volume": self.cfg.rosalina_volume, "todo": False},
        )

    # -- stage 2: deploy ---------------------------------------------------- #
    def deploy(self, local_bcsar: str | Path, *, remote: Optional[str] = None) -> StageResult:
        """ftp_put the cartridge, ftp_get it back, and hash-compare to confirm."""
        local_bcsar = Path(local_bcsar)
        if not local_bcsar.is_file():
            return StageResult("deploy", False, f"local .bcsar not found: {local_bcsar}", {})
        remote = remote or self.cfg.luma_bcsar_path
        ip = self.cfg.n3ds.require_ip()
        port = self.cfg.n3ds.ftp_port

        local_hash = hashlib.sha256(local_bcsar.read_bytes()).hexdigest()
        try:
            self._ftp_put(ip, str(local_bcsar), remote, port)
        except Exception as exc:
            return StageResult("deploy", False, f"ftp_put failed: {exc}",
                               {"remote": remote, "local_sha256": local_hash})

        readback = self.cfg.captures_dir / (local_bcsar.stem + ".readback.bcsar")
        readback.parent.mkdir(parents=True, exist_ok=True)
        try:
            self._ftp_get(ip, remote, str(readback), port)
        except Exception as exc:
            return StageResult("deploy", False, f"ftp_get readback failed: {exc}",
                               {"remote": remote, "local_sha256": local_hash})

        remote_hash = hashlib.sha256(Path(readback).read_bytes()).hexdigest()
        data = {"remote": remote, "local_sha256": local_hash,
                "remote_sha256": remote_hash, "readback": str(readback)}
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

    sub.add_parser("preflight", help="ftpd + NTR health and volume re-apply.")

    d = sub.add_parser("deploy", help="Push a cartridge .bcsar and hash-verify it.")
    d.add_argument("bcsar")

    n = sub.add_parser("navigate", help="Navigate to a plaza track (screenshot-verified).")
    n.add_argument("--track", default="Main Theme 1", choices=sorted(PLAZA_TRACKS))

    r = sub.add_parser("record", help="Record N passes and level-check.")
    r.add_argument("out")
    r.add_argument("--passes", type=int, default=2)
    r.add_argument("--seconds-per-pass", type=float, default=None)

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
            res = sess.record(args.out, passes=args.passes,
                              seconds_per_pass=args.seconds_per_pass)
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
