"""Offline unit tests for the console-capture harness.

Everything runs against the n3ds-mcp simulators (SimInputSink / SimNTR), fake ftp
+ fake ffmpeg, and canned verdict runners -- NO live console, NO real ffmpeg.
Run with the n3ds-mcp venv Python (see conftest.py):

    E:\\GitHub\\3ds-mcp\\.venv\\Scripts\\python.exe -m pytest tools/console-capture/tests -q

Coverage mirrors the task's required cases: deploy hash-mismatch detection,
navigation checkpoint failure aborts, record level-assertion catches silence +
clipping (+ dead channel, hot, noisy floor), the pass-count -> seconds math, the
verdict exit-code mapping, and the NTR-on/off perturbation A/B.
"""

from __future__ import annotations

import math
import socket
import struct
import wave
from array import array
from pathlib import Path

import pytest

import console_capture as cc
from console_capture import (
    CaptureConfig,
    ConsoleCaptureSession,
    ExpectScreen,
    NavStep,
    RosalinaMenuDriver,
    Tap,
    analyze_levels,
    assert_capture_levels,
    build_ffmpeg_cmd,
    plaza_path_for_track,
    record_seconds,
)

from n3ds_mcp.input_redirect import InputClient
from n3ds_mcp.ntr import NTRClient
from n3ds_mcp.rosalina_read import RosalinaRow, RosalinaScreen
from sim_input import SimInputSink
from sim_ntr import SimNTR


# --------------------------------------------------------------------------- #
# Synthetic WAV fixtures (model the real capture chain: quiet lead-in + body)
# --------------------------------------------------------------------------- #
def _clip16(x: float) -> int:
    return max(-32768, min(32767, int(round(x))))


def write_synth_wav(path: Path, kind: str = "good", rate: int = 48000) -> Path:
    """Deterministic stereo 16-bit fixture. A ~0.3 s quiet lead-in (the analog
    noise floor) + a ~0.5 s body, so the minimum-window noise-floor metric reads
    the lead-in and the peak/RMS read the body -- exactly like a battery capture.
    """
    import random

    rng = random.Random(1234)
    lead, body = int(0.3 * rate), int(0.5 * rate)
    floor_dbfs = -48.0 if kind == "noisy" else -70.0

    def noise(nsamp: int, dbfs: float) -> list[float]:
        amp = (10.0 ** (dbfs / 20.0)) * math.sqrt(3.0) * 32768.0  # uniform -> rms
        return [rng.uniform(-amp, amp) for _ in range(nsamp)]

    def sine(nsamp: int, amp: float, freq: float = 440.0) -> list[float]:
        return [amp * 32768.0 * math.sin(2.0 * math.pi * freq * i / rate) for i in range(nsamp)]

    left = noise(lead, floor_dbfs)
    right = noise(lead, floor_dbfs)
    if kind == "good":
        left += sine(body, 0.5); right += sine(body, 0.5)
    elif kind == "good_lo":                       # a hair quieter, still in-window
        left += sine(body, 0.4); right += sine(body, 0.4)
    elif kind == "hot":                           # too hot: outside window, not clipping
        left += sine(body, 0.9); right += sine(body, 0.9)
    elif kind == "clipping":                      # over the rail
        left += sine(body, 1.5); right += sine(body, 1.5)
    elif kind == "dead_right":                    # right channel absent
        left += sine(body, 0.5); right += noise(body, floor_dbfs)
    elif kind == "noisy":                         # good body but a lifted floor
        left += sine(body, 0.5); right += sine(body, 0.5)
    elif kind == "silence":                       # nothing but floor everywhere
        left += noise(body, floor_dbfs); right += noise(body, floor_dbfs)
    else:
        raise ValueError(f"unknown fixture kind {kind!r}")

    buf = array("h")
    for l, r in zip(left, right):
        buf.append(_clip16(l)); buf.append(_clip16(r))
    with wave.open(str(path), "wb") as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(rate)
        w.writeframes(buf.tobytes())
    return path


# --------------------------------------------------------------------------- #
# Fakes
# --------------------------------------------------------------------------- #
class FakeFtp:
    """In-memory ftp store with n3ds devtools signatures. ``corrupt`` mutates on
    STOR so the readback hash won't match (a bad push)."""

    def __init__(self, corrupt: bool = False, unreachable: bool = False) -> None:
        self.store: dict[str, bytes] = {}
        self.corrupt = corrupt
        self.unreachable = unreachable
        self.puts = 0

    def put(self, ip, local, remote, port=5000):
        if self.unreachable:
            raise RuntimeError("Could not reach ftpd")
        self.puts += 1
        data = Path(local).read_bytes()
        self.store[remote] = data + (b"\x00" if self.corrupt else b"")
        return remote

    def get(self, ip, remote, local, port=5000):
        if remote not in self.store:
            raise RuntimeError(f"no such remote {remote}")
        Path(local).parent.mkdir(parents=True, exist_ok=True)
        Path(local).write_bytes(self.store[remote])
        return str(Path(local).resolve())

    def list(self, ip, path="/", port=5000):
        if self.unreachable:
            raise RuntimeError("Could not reach ftpd")
        return ["luma/", "3ds/"]


class FakeRecorder:
    """Writes a synthetic WAV instead of running ffmpeg. ``kinds`` may be a single
    kind or a per-call list; records the seconds/out of every call."""

    def __init__(self, kinds="good") -> None:
        self.kinds = [kinds] if isinstance(kinds, str) else list(kinds)
        self.calls: list[tuple[float, str]] = []

    def record(self, cfg, seconds, out_path):
        kind = self.kinds[min(len(self.calls), len(self.kinds) - 1)]
        self.calls.append((seconds, str(out_path)))
        write_synth_wav(Path(out_path), kind, cfg.sample_rate)


class FakeFrame:
    """Minimal stand-in for a PIL frame (AcceptingVerifier only checks non-None)."""


class FakeNTR:
    def __init__(self) -> None:
        self.starts = 0
        self.stops = 0
        self._connected = True

    def start(self):
        self.starts += 1
        self._connected = True

    def stop(self):
        self.stops += 1
        self._connected = False

    def get_frame(self, screen="top"):
        return (FakeFrame(), 0.01)

    def status(self):
        return {"connected": self._connected, "fps": 30.0, "top_age_s": 0.01, "bottom_age_s": 0.01}


class FailAtVerifier:
    """Rejects the frame at a given (0-based) checkpoint index."""

    def __init__(self, fail_index: int) -> None:
        self.fail_index = fail_index
        self._i = -1

    def verify(self, step, frame):
        self._i += 1
        if self._i == self.fail_index:
            return False, f"forced failure at step '{step.name}'"
        return True, "ok"


class FakeRosalinaMenu:
    """Offline state machine of the Rosalina overlay's volume walk.

    Implements BOTH collaborator surfaces of :class:`RosalinaMenuDriver` -- the
    reader (``read_once``; the driver polls it directly and never calls the
    library's ``wait_for_change``) and the input client (``press`` /
    ``set_state`` / ``neutral``) -- so the REAL driver code runs end-to-end
    against it. Mirrors the live-observed layout (2026-07-18): the
    root cursor resets to the top entry on open, "System configuration..." leads
    to a submenu whose first entry is "Control volume", Y toggles ENABLED (the
    Value row only exists while enabled), DLEFT/DRIGHT step +/-10 and DUP/DDOWN
    +/-1 (clamped 0..100), A applies and shows "Success!", and the MENU_CLOSE
    interface bit closes from any depth.
    """

    ROOT = ["Take screenshot", "Screen filters...", "Cheats...",
            "Plugin Loader: [Disabled]", "New 3DS menu...", "Process list",
            "Debugger options...", "System configuration...",
            "Miscellaneous options...", "n3ds-mcp development mode",
            "Save settings", "Return To HOME Menu", "Power off / reboot",
            "System info", "Credits", "Debug info"]
    SYSCONFIG = ["Control volume", "Control Wireless connection", "Toggle LEDs",
                 "Toggle Wireless", "Toggle Power Button",
                 "Toggle power to card slot", "Change screen brightness"]

    def __init__(self, enabled: bool = False, value: int = 85,
                 on_screenshot=None) -> None:
        self.state = "closed"   # closed | root | sysconfig | volume
        self.cursor = 0
        self.enabled = enabled
        self.value = value
        self.success = False
        self.epoch = 100
        self.press_log: list[frozenset] = []
        # Fired when A activates "Take screenshot" -- models the SD write of the
        # framebuffer BMPs (the real menu writes them to /luma/screenshots/).
        self._on_screenshot = on_screenshot
        self.screenshots_taken = 0

    # -- input-client surface ----------------------------------------------- #
    def press(self, buttons, hold_ms: int = 120) -> None:
        keys = {str(b).upper() for b in buttons}
        self.press_log.append(frozenset(keys))
        self._apply(keys)

    def set_state(self, pressed=frozenset(), touch=None, circle=(0.0, 0.0),
                  cstick=(0.0, 0.0), interface=frozenset()) -> None:
        if "MENU_CLOSE" in interface and self.state != "closed":
            self.state = "closed"
            self.epoch += 1

    def neutral(self) -> None:
        pass

    def _apply(self, keys: set) -> None:
        if self.state == "closed":
            if {"L", "DDOWN", "SELECT"} <= keys:
                self.state = "root"
                self.cursor = 0
                self.epoch += 1
            return
        self.epoch += 1
        if self.state == "root":
            if "DDOWN" in keys:
                self.cursor = min(self.cursor + 1, len(self.ROOT) - 1)
            elif "DUP" in keys:
                self.cursor = max(self.cursor - 1, 0)
            elif "A" in keys and self.ROOT[self.cursor] == "System configuration...":
                self.state = "sysconfig"
                self.cursor = 0
            elif "A" in keys and self.ROOT[self.cursor] == "Take screenshot":
                self.screenshots_taken += 1
                if self._on_screenshot is not None:
                    self._on_screenshot()
        elif self.state == "sysconfig":
            if "DDOWN" in keys:
                self.cursor = min(self.cursor + 1, len(self.SYSCONFIG) - 1)
            elif "DUP" in keys:
                self.cursor = max(self.cursor - 1, 0)
            elif "A" in keys and self.SYSCONFIG[self.cursor] == "Control volume":
                self.state = "volume"
                self.success = False
        elif self.state == "volume":
            if "Y" in keys:
                self.enabled = not self.enabled
                self.success = False
            elif self.enabled and keys & {"DLEFT", "DRIGHT", "DUP", "DDOWN"}:
                step = (10 if "DRIGHT" in keys else -10 if "DLEFT" in keys
                        else 1 if "DUP" in keys else -1)
                self.value = max(0, min(100, self.value + step))
                self.success = False
            elif "A" in keys:
                self.success = True

    # -- reader surface ------------------------------------------------------ #
    def read_once(self, planes: int = 0, **kw) -> RosalinaScreen:
        rows: list[RosalinaRow] = []
        if self.state == "root":
            rows.append(RosalinaRow(10, "   Rosalina menu"))
            for i, e in enumerate(self.ROOT):
                rows.append(RosalinaRow(30 + 11 * i,
                                        ("  >  " if i == self.cursor else "     ") + e))
        elif self.state == "sysconfig":
            rows.append(RosalinaRow(10, "   System configuration menu"))
            for i, e in enumerate(self.SYSCONFIG):
                rows.append(RosalinaRow(30 + 11 * i,
                                        ("  >  " if i == self.cursor else "     ") + e))
        elif self.state == "volume":
            rows.append(RosalinaRow(10, "   System configuration menu"))
            rows.append(RosalinaRow(30, "  Y: Toggle volume slider override."))
            rows.append(RosalinaRow(41, "  DPAD/CPAD: Adjust the volume level."))
            rows.append(RosalinaRow(52, "  A: Apply"))
            rows.append(RosalinaRow(63, "  B: Go back"))
            rows.append(RosalinaRow(85, "  Current status: "
                                    + ("ENABLED" if self.enabled else "DISABLED")))
            if self.enabled:
                rows.append(RosalinaRow(96, f"     Value: [{self.value}%]"))
            if self.success:
                rows.append(RosalinaRow(107, "  Success!"))
        return RosalinaScreen(overlay_active=self.state != "closed",
                              epoch=self.epoch, torn=False,
                              rows_overflowed=False, truncated=False, rows=rows)


def _make_bmp_bytes(size=(4, 4), color=(20, 40, 60)) -> bytes:
    """A real, PIL-decodable uncompressed BMP -- stands in for what Rosalina's
    'Take screenshot' writes to /luma/screenshots/."""
    import io

    from PIL import Image

    buf = io.BytesIO()
    Image.new("RGB", size, color).save(buf, format="BMP")
    return buf.getvalue()


class FakeScreenshotFtp:
    """Path-aware fake for the Rosalina screenshot fetch: lists BMP basenames
    under /luma/screenshots and serves their bytes on RETR. Records every get."""

    DIR = "/luma/screenshots"

    def __init__(self, bmps: dict) -> None:
        self.bmps = dict(bmps)          # {basename: bytes}
        self.gets: list[str] = []

    def add(self, name: str, data: bytes) -> None:
        """Materialise a screenshot BMP (what the menu's 'Take screenshot' does)."""
        self.bmps[name] = data

    def list(self, ip, path="/", port=4952):
        if path.rstrip("/") == self.DIR:
            return sorted(self.bmps)
        raise RuntimeError(f"no such dir {path}")

    def get(self, ip, remote, local, port=4952):
        name = remote.rsplit("/", 1)[-1]
        if name not in self.bmps:
            raise RuntimeError(f"no such remote {remote}")
        Path(local).parent.mkdir(parents=True, exist_ok=True)
        Path(local).write_bytes(self.bmps[name])
        self.gets.append(remote)
        return str(Path(local).resolve())


def _rosa_source(tmp_path, menu, ftp, *, is_recording=lambda: False):
    """A RosalinaScreenshotSource driving the fake menu + fake screenshot ftp,
    with the SD-write settle zeroed so tests don't sleep."""
    drv = RosalinaMenuDriver(menu, menu)
    return cc.RosalinaScreenshotSource(
        drv, ftp.get, ftp.list, "127.0.0.1", 4952,
        screenshots_dir=tmp_path / "shots",
        is_recording=is_recording, screenshot_settle_s=0.0)


def free_udp_port() -> int:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class SimRig:
    """Real InputClient + NTRClient wired to the n3ds-mcp simulators."""

    def __init__(self, ntr_program=None) -> None:
        self.sink = SimInputSink(port=0)
        self.video_port = free_udp_port()
        self.sim = SimNTR(tcp_port=0, target_udp_port=self.video_port)
        (ntr_program or (lambda s: s.default_program()))(self.sim)
        self.sim.start()
        self.input = InputClient("127.0.0.1", self.sink.port)
        self.ntr = NTRClient("127.0.0.1", control_port=self.sim.tcp_port,
                             video_port=self.video_port)
        self.ntr.start()

    def wait_first_frame(self, timeout=6.0) -> bool:
        import time
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.ntr.get_frame("top") is not None:
                return True
            time.sleep(0.02)
        return False

    def close(self):
        self.ntr.stop()
        self.input.close()
        self.sim.stop()
        self.sink.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def _cfg(tmp_path, **kw) -> CaptureConfig:
    cfg = CaptureConfig(captures_dir=tmp_path)
    cfg.n3ds.ip = "127.0.0.1"
    for k, v in kw.items():
        setattr(cfg, k, v)
    return cfg


# =========================================================================== #
# Pure helpers
# =========================================================================== #
def test_record_seconds_math():
    assert record_seconds(2, 23.5) == pytest.approx(47.0)
    assert record_seconds(3, 23.5, lead_in_s=1.0) == pytest.approx(71.5)
    assert record_seconds(1, 10.0) == pytest.approx(10.0)
    with pytest.raises(ValueError):
        record_seconds(0, 23.5)


def test_ffmpeg_command_shape(tmp_path):
    cfg = _cfg(tmp_path)
    cmd = build_ffmpeg_cmd(cfg, 47.0, tmp_path / "out.wav")
    assert cmd[0] == "ffmpeg"
    # the dshow device is passed as a single `audio=<name>` argv token
    assert "-f" in cmd and cmd[cmd.index("-f") + 1] == "dshow"
    assert f"audio={cc.AUDIO_DEVICE}" in cmd
    assert cmd[cmd.index("-t") + 1] == "47.000"
    assert cmd[cmd.index("-ar") + 1] == "48000"
    assert cmd[cmd.index("-ac") + 1] == "2"
    assert cmd[cmd.index("-c:a") + 1] == "pcm_s16le"
    assert str(tmp_path / "out.wav") == cmd[-1]


# =========================================================================== #
# Level analysis / assertion
# =========================================================================== #
def test_levels_good_passes(tmp_path):
    w = write_synth_wav(tmp_path / "g.wav", "good")
    rep = analyze_levels(w)
    assert rep.channels == 2
    res = assert_capture_levels(rep, _cfg(tmp_path))
    assert res.ok, res.message
    # both channels around -6 dBFS, floor well below the threshold
    for p in rep.peak_dbfs:
        assert -9.0 <= p <= -3.0
    assert max(rep.noise_floor_dbfs) < -55.0


def test_levels_silence_fails(tmp_path):
    w = write_synth_wav(tmp_path / "s.wav", "silence")
    res = assert_capture_levels(analyze_levels(w), _cfg(tmp_path))
    assert not res.ok
    assert "silent" in res.message.lower()


def test_levels_clipping_fails(tmp_path):
    w = write_synth_wav(tmp_path / "c.wav", "clipping")
    res = assert_capture_levels(analyze_levels(w), _cfg(tmp_path))
    assert not res.ok
    assert "clip" in res.message.lower()


def test_levels_dead_channel_fails(tmp_path):
    w = write_synth_wav(tmp_path / "d.wav", "dead_right")
    rep = analyze_levels(w)
    res = assert_capture_levels(rep, _cfg(tmp_path))
    assert not res.ok
    assert "R silent" in res.message or "absent" in res.message
    assert rep.channel_present(0) and not rep.channel_present(1)


def test_levels_too_hot_fails_window(tmp_path):
    w = write_synth_wav(tmp_path / "h.wav", "hot")
    res = assert_capture_levels(analyze_levels(w), _cfg(tmp_path))
    assert not res.ok
    assert "outside window" in res.message


def test_levels_noisy_floor_fails(tmp_path):
    w = write_synth_wav(tmp_path / "n.wav", "noisy")
    rep = analyze_levels(w)
    res = assert_capture_levels(rep, _cfg(tmp_path))
    assert not res.ok
    assert "noise floor" in res.message
    # peaks are still fine -- only the floor is the problem
    for p in rep.peak_dbfs:
        assert -9.0 <= p <= -3.0


# =========================================================================== #
# Stage 2: deploy (hash verify)
# =========================================================================== #
def _write_bcsar(tmp_path, name="cart.bcsar", body=b"BCSAR\x00fake-cartridge-bytes") -> Path:
    p = tmp_path / name
    p.write_bytes(body)
    return p


def test_deploy_happy_path_hash_matches(tmp_path):
    ftp = FakeFtp()
    cfg = _cfg(tmp_path)
    sess = ConsoleCaptureSession(cfg, ftp_put=ftp.put, ftp_get=ftp.get, ftp_list=ftp.list)
    local = _write_bcsar(tmp_path)
    res = sess.deploy(local)
    assert res.ok, res.message
    assert res.data["local_sha256"] == res.data["remote_sha256"]
    assert cfg.luma_bcsar_path in ftp.store


def test_deploy_detects_hash_mismatch(tmp_path):
    ftp = FakeFtp(corrupt=True)
    sess = ConsoleCaptureSession(_cfg(tmp_path), ftp_put=ftp.put, ftp_get=ftp.get, ftp_list=ftp.list)
    res = sess.deploy(_write_bcsar(tmp_path))
    assert not res.ok
    assert "mismatch" in res.message.lower()
    assert res.data["local_sha256"] != res.data["remote_sha256"]


def test_deploy_missing_local_file_fails(tmp_path):
    sess = ConsoleCaptureSession(_cfg(tmp_path), ftp_put=FakeFtp().put, ftp_get=FakeFtp().get)
    res = sess.deploy(tmp_path / "does-not-exist.bcsar")
    assert not res.ok
    assert "not found" in res.message.lower()


def test_deploy_verify_first_skips_push_when_identical(tmp_path):
    # The boot-time file service is write-confined to /luma/staging/, so an
    # already-installed identical cartridge must verify WITHOUT a push.
    ftp = FakeFtp()
    cfg = _cfg(tmp_path)
    local = _write_bcsar(tmp_path)
    ftp.store[cfg.luma_bcsar_path] = local.read_bytes()  # pre-installed, identical
    sess = ConsoleCaptureSession(cfg, ftp_put=ftp.put, ftp_get=ftp.get, ftp_list=ftp.list)
    res = sess.deploy(local)
    assert res.ok, res.message
    assert res.data["pushed"] is False
    assert "no push needed" in res.message
    assert ftp.puts == 0


def test_deploy_targets_the_titles_lane_on_the_file_service_port(tmp_path):
    # deploy pushes over the file service (default port 4952) to a /luma/titles/
    # content-lane path -- no ftpd:5000 leg, no staging confinement.
    ftp = FakeFtp()
    cfg = _cfg(tmp_path)
    assert cfg.n3ds.ftp_port == 4952
    assert cfg.luma_bcsar_path.startswith("/luma/titles/")
    sess = ConsoleCaptureSession(cfg, ftp_put=ftp.put, ftp_get=ftp.get, ftp_list=ftp.list)
    res = sess.deploy(_write_bcsar(tmp_path))
    assert res.ok, res.message
    assert cfg.luma_bcsar_path in ftp.store


def test_deploy_missing_titles_dir_gets_actionable_hint(tmp_path):
    # The /luma/titles/ lane has no MKD: a STOR into a path whose parent dir was
    # never created fails, and listing that parent fails too. The hint must name
    # the missing directory and the no-MKD constraint (NOT the retired ftpd:5000).
    class NoDirFtp(FakeFtp):
        def put(self, ip, local, remote, port=4952):
            raise RuntimeError("552 Write failed")

        def list(self, ip, path="/", port=4952):
            raise RuntimeError("550 Not a directory")  # the parent dir is absent

    ftp = NoDirFtp()
    sess = ConsoleCaptureSession(_cfg(tmp_path), ftp_put=ftp.put, ftp_get=ftp.get,
                                 ftp_list=ftp.list)
    res = sess.deploy(_write_bcsar(tmp_path))
    assert not res.ok
    assert "does not exist" in res.message and "MKD" in res.message
    assert "N3DS_FTP_PORT=5000" not in res.message  # ftpd is retired for deploy


def test_deploy_stor_failure_with_existing_dir_gets_lane_hint(tmp_path):
    # STOR fails but the parent directory lists fine -> not a missing-dir case;
    # the generic content-lane hint is shown, with no misleading ftpd advice.
    class StorFailFtp(FakeFtp):
        def put(self, ip, local, remote, port=4952):
            raise RuntimeError("552 Write failed")

    ftp = StorFailFtp()  # .list returns a non-empty listing -> dir "exists"
    sess = ConsoleCaptureSession(_cfg(tmp_path), ftp_put=ftp.put, ftp_get=ftp.get,
                                 ftp_list=ftp.list)
    res = sess.deploy(_write_bcsar(tmp_path))
    assert not res.ok
    assert "/luma/titles/" in res.message
    assert "5000" not in res.message  # on the default port, no legacy-ftpd advice


def test_deploy_refuses_oversize_before_transfer(tmp_path):
    # The 16 MiB content-lane ceiling is enforced CLIENT-SIDE, before any bytes
    # move -- an oversize STOR would otherwise stream the whole file (~149 s for
    # 16 MiB) only to be refused. Nothing hits the wire here.
    ftp = FakeFtp()
    cfg = _cfg(tmp_path, max_content_bytes=16)       # tiny ceiling for the test
    local = _write_bcsar(tmp_path, body=b"X" * 64)   # 64 B > 16 B ceiling
    sess = ConsoleCaptureSession(cfg, ftp_put=ftp.put, ftp_get=ftp.get, ftp_list=ftp.list)
    res = sess.deploy(local)
    assert not res.ok
    assert "over the" in res.message and "ceiling" in res.message
    assert ftp.puts == 0                # never streamed
    assert res.data["size_bytes"] == 64


def test_deploy_allows_cartridge_exactly_at_ceiling(tmp_path):
    # The guard is strictly greater-than: a cartridge exactly at the ceiling
    # deploys normally.
    ftp = FakeFtp()
    cfg = _cfg(tmp_path, max_content_bytes=64)
    local = _write_bcsar(tmp_path, body=b"X" * 64)
    sess = ConsoleCaptureSession(cfg, ftp_put=ftp.put, ftp_get=ftp.get, ftp_list=ftp.list)
    res = sess.deploy(local)
    assert res.ok, res.message
    assert ftp.puts == 1


def test_deploy_refuses_write_while_ntr_streaming(tmp_path):
    # A sustained STOR dies within seconds under an active NTR stream; when a push
    # is actually needed and NTR is up, deploy refuses fast rather than streaming
    # a few KB and dying with a socket reset.
    ftp = FakeFtp()  # empty store -> RETR fails -> bytes "differ" -> write path
    sess = ConsoleCaptureSession(_cfg(tmp_path), ntr_client=FakeNTR(),
                                 ftp_put=ftp.put, ftp_get=ftp.get, ftp_list=ftp.list)
    res = sess.deploy(_write_bcsar(tmp_path))
    assert not res.ok
    assert "NTR" in res.message and "stream" in res.message.lower()
    assert res.data["ntr_streaming"] is True
    assert ftp.puts == 0  # never streamed a byte


def test_deploy_verify_only_succeeds_while_ntr_streaming(tmp_path):
    # RETR is unaffected by NTR, so an already-deployed cartridge still verifies
    # with the stream up -- the guard is only on the write path.
    ftp = FakeFtp()
    cfg = _cfg(tmp_path)
    local = _write_bcsar(tmp_path)
    ftp.store[cfg.luma_bcsar_path] = local.read_bytes()  # pre-installed, identical
    sess = ConsoleCaptureSession(cfg, ntr_client=FakeNTR(),
                                 ftp_put=ftp.put, ftp_get=ftp.get, ftp_list=ftp.list)
    res = sess.deploy(local)
    assert res.ok, res.message
    assert res.data["pushed"] is False
    assert ftp.puts == 0


# =========================================================================== #
# Stage 3: navigate (against the real sims)
# =========================================================================== #
def _short_path(n_steps=3, screen="top"):
    return [
        NavStep(f"step{i}", (Tap(("A",), hold_ms=5, wait_ms=3),),
                ExpectScreen(f"cp{i}", screen), settle_ms=3)
        for i in range(n_steps)
    ]


def test_navigate_composes_against_sims_and_reaches_all_checkpoints(tmp_path):
    with SimRig() as rig:
        assert rig.wait_first_frame(), "sim never streamed a frame"
        sess = ConsoleCaptureSession(_cfg(tmp_path), input_client=rig.input, ntr_client=rig.ntr)
        path = _short_path(3)
        res = sess.navigate(path)
        assert res.ok, res.message
        assert len(res.data["checkpoints"]) == 3
        assert all(c["ok"] for c in res.data["checkpoints"])
        # every tap reached the input sink: 3 steps x (press + release) = 6 packets
        assert rig.sink.wait_for(6, timeout=2.0), f"only {rig.sink.count()} packets"
        pressed = [p["pressed"] for p in rig.sink.packets if p["pressed"]]
        assert pressed == [{"A"}, {"A"}, {"A"}]


def test_navigate_checkpoint_failure_aborts(tmp_path):
    with SimRig() as rig:
        assert rig.wait_first_frame()
        sess = ConsoleCaptureSession(_cfg(tmp_path), input_client=rig.input, ntr_client=rig.ntr)
        path = _short_path(3)
        res = sess.navigate(path, verifier=FailAtVerifier(fail_index=1))
        assert not res.ok
        assert res.data["failed_index"] == 1
        assert res.data["failed_step"] == "step1"
        # steps 0 and 1 ran (2 taps -> 4 packets); step 2 never fired
        assert rig.sink.wait_for(4, timeout=2.0)
        import time
        time.sleep(0.15)  # give any stray packet time to (not) arrive
        assert rig.sink.count() == 4, "aborted nav must not run the step after the failure"


def test_plaza_path_structure_for_battery_a():
    # "Main Theme 1" is battery A -> one DDOWN into the track list.
    path = plaza_path_for_track("Main Theme 1")
    names = [s.name for s in path]
    assert names == ["select-plaza-tile", "launch-plaza", "enter-plaza",
                     "clear-modals", "switch-icons", "select-music-player",
                     "open-music-player", "select-track", "play"]
    all_taps = [tap for s in path for tap in s.taps]
    all_buttons = [tap.buttons for tap in all_taps]
    # HOME opens the menu; DLEFT saturates to the leftmost tile; 2x DRIGHT
    # lands on the plaza tile (3rd tile in the 2026-07-19 layout -- the HOME
    # layout is user-mutable, the banner checkpoint is the real guard)
    assert ("HOME",) in all_buttons
    assert sum(1 for b in all_buttons if b == ("DLEFT",)) == 12
    assert sum(1 for b in all_buttons if b == ("DRIGHT",)) == 2
    # Y switches the bottom grid to features; the Music Player is a touch tap
    assert sum(1 for b in all_buttons if b == ("Y",)) == 1
    touches = [tap.touch for tap in all_taps if tap.touch is not None]
    assert touches == [cc.MUSIC_PLAYER_TOUCH_XY]
    # the track is one DDOWN
    assert sum(1 for b in all_buttons if b == ("DDOWN",)) == 1

    # "Entrance" is the top of the list -> no select-track step (0 DDOWN)
    entrance = plaza_path_for_track("Entrance")
    assert "select-track" not in [s.name for s in entrance]

    with pytest.raises(KeyError):
        plaza_path_for_track("No Such Track")


# =========================================================================== #
# Stage 4: record (level assertion inline)
# =========================================================================== #
def test_record_good_passes_and_uses_correct_seconds(tmp_path):
    rec = FakeRecorder("good")
    sess = ConsoleCaptureSession(_cfg(tmp_path), recorder=rec)
    res = sess.record("take.wav", passes=2, seconds_per_pass=23.5)
    assert res.ok, res.message
    assert res.data["seconds"] == pytest.approx(47.0)
    assert rec.calls and rec.calls[0][0] == pytest.approx(47.0)
    assert Path(res.data["out"]).is_file()


def test_record_catches_silence(tmp_path):
    sess = ConsoleCaptureSession(_cfg(tmp_path), recorder=FakeRecorder("silence"))
    res = sess.record("take.wav", passes=1, seconds_per_pass=1.0)
    assert not res.ok
    assert "silent" in res.message.lower()


def test_record_catches_clipping(tmp_path):
    sess = ConsoleCaptureSession(_cfg(tmp_path), recorder=FakeRecorder("clipping"))
    res = sess.record("take.wav", passes=1, seconds_per_pass=1.0)
    assert not res.ok
    assert "clip" in res.message.lower()


def test_record_can_skip_level_assertion(tmp_path):
    sess = ConsoleCaptureSession(_cfg(tmp_path), recorder=FakeRecorder("clipping"))
    res = sess.record("take.wav", passes=1, seconds_per_pass=1.0, assert_levels=False)
    assert res.ok  # clipping ignored when the assertion is off


# =========================================================================== #
# Stage 5: verdict (exit-code mapping)
# =========================================================================== #
def _verdict_sess(tmp_path, exit_code, captured):
    def runner(argv):
        captured.append(argv)
        return exit_code, f"stdout for exit {exit_code}", "stderr"
    return ConsoleCaptureSession(_cfg(tmp_path), verdict_runner=runner)


def test_verdict_pass(tmp_path):
    captured: list = []
    sess = _verdict_sess(tmp_path, 0, captured)
    res = sess.verdict(tmp_path / "cap.wav", tmp_path / "render.wav")
    assert res.ok and "PASS" in res.message
    # the analyzer was invoked with capture + render paths
    assert str(tmp_path / "cap.wav") in captured[0]
    assert str(tmp_path / "render.wav") in captured[0]


def test_verdict_out_of_tolerance(tmp_path):
    sess = _verdict_sess(tmp_path, 1, [])
    res = sess.verdict(tmp_path / "cap.wav", tmp_path / "render.wav")
    assert not res.ok and "out of tolerance" in res.message.lower()


def test_verdict_harness_error_is_not_a_pass(tmp_path):
    sess = _verdict_sess(tmp_path, 2, [])
    res = sess.verdict(tmp_path / "cap.wav", tmp_path / "render.wav")
    assert not res.ok
    assert "harness error" in res.message.lower()


# =========================================================================== #
# Stage 1: preflight + the Rosalina volume-override walk
# =========================================================================== #
def test_preflight_healthy_skips_volume_when_unconfigured(tmp_path):
    ftp = FakeFtp()
    ntr = FakeNTR()
    # rosalina_volume=None -> the analog slider is authoritative; the re-apply
    # SKIPS with a notice and never fails the preflight.
    sess = ConsoleCaptureSession(_cfg(tmp_path, rosalina_volume=None),
                                 ntr_client=ntr, ftp_list=ftp.list)
    res = sess.preflight()
    assert res.ok, res.message
    assert res.data["volume"]["skipped"] is True
    assert res.data["volume"]["applied"] is False


def test_preflight_file_service_unreachable_fails(tmp_path):
    ftp = FakeFtp(unreachable=True)
    sess = ConsoleCaptureSession(_cfg(tmp_path), ntr_client=FakeNTR(), ftp_list=ftp.list)
    res = sess.preflight()
    assert not res.ok
    assert "file service unreachable" in res.message.lower()


def _no_deadzone(monkeypatch):
    # the post-close input deadzone is a live-console fact; don't sleep in tests
    monkeypatch.setattr(cc, "POST_MENU_CLOSE_DEADZONE_S", 0.0)


def test_preflight_applies_volume_when_configured(tmp_path, monkeypatch):
    _no_deadzone(monkeypatch)
    menu = FakeRosalinaMenu(enabled=False, value=85)
    cfg = _cfg(tmp_path, rosalina_volume=80)
    sess = ConsoleCaptureSession(cfg, ntr_client=FakeNTR(), ftp_list=FakeFtp().list,
                                 rosalina_driver=RosalinaMenuDriver(menu, menu))
    res = sess.preflight()
    assert res.ok, res.message
    assert res.data["volume"]["applied"] is True
    assert res.data["volume"]["volume"] == 80
    # the fake console really was walked there: enabled, pinned, and closed
    assert menu.enabled is True
    assert menu.value == 80
    assert menu.success is True
    assert menu.state == "closed"


def test_volume_walk_from_arbitrary_start(monkeypatch):
    _no_deadzone(monkeypatch)
    # already ENABLED at 42 -> no Y press, closed-loop step to 65
    menu = FakeRosalinaMenu(enabled=True, value=42)
    drv = RosalinaMenuDriver(menu, menu)
    out = drv.set_volume_override(65)
    assert out == {"value": 65, "enabled": True, "applied": True}
    assert menu.value == 65 and menu.state == "closed"
    # no Y was pressed (it was already enabled)
    assert frozenset({"Y"}) not in menu.press_log


def test_volume_walk_closes_overlay_even_on_failure(monkeypatch):
    _no_deadzone(monkeypatch)

    class BrokenApply(FakeRosalinaMenu):
        def _apply(self, keys):
            # the A on the volume screen never confirms (no Success! row)
            if self.state == "volume" and "A" in keys:
                self.epoch += 1
                return
            super()._apply(keys)

    menu = BrokenApply(enabled=True, value=65)
    drv = RosalinaMenuDriver(menu, menu)
    with pytest.raises(RuntimeError, match="apply not confirmed"):
        drv.set_volume_override(65)
    # the overlay must NOT be left open (the console would stay frozen)
    assert menu.state == "closed"


def test_volume_step_buttons_minimal_walks():
    f = RosalinaMenuDriver.volume_step_buttons
    assert f(65, 65) == []
    assert f(85, 65) == ["DLEFT", "DLEFT"]
    assert f(60, 65) == ["DUP"] * 5
    # -5: plain ones beat a tens overshoot (5 presses, not 6)
    assert f(70, 65) == ["DDOWN"] * 5
    # +17: overshoot by tens then walk back (5 presses, not 8)
    assert f(48, 65) == ["DRIGHT", "DRIGHT", "DDOWN", "DDOWN", "DDOWN"]


def test_parse_volume_screen():
    parse = RosalinaMenuDriver.parse_volume_screen
    menu = FakeRosalinaMenu(enabled=True, value=65)
    menu.state = "volume"
    assert parse(menu.read_once()) == (True, 65)
    menu.enabled = False
    assert parse(menu.read_once()) == (False, None)  # no Value row while disabled
    menu.state = "root"
    assert parse(menu.read_once()) == (None, None)   # not the volume screen


# =========================================================================== #
# Stage 6: perturbation A/B (NTR streaming on/off)
# =========================================================================== #
def test_perturbation_ab_pass_when_identical(tmp_path):
    ntr = FakeNTR()
    rec = FakeRecorder("good")  # both takes identical -> no perturbation
    sess = ConsoleCaptureSession(_cfg(tmp_path), ntr_client=ntr, recorder=rec)
    res = sess.perturbation_ab(passes=1, seconds_per_pass=1.0)
    assert res.ok, res.message
    assert res.data["worst_db"] == pytest.approx(0.0, abs=1e-6)
    assert ntr.stops >= 1, "NTR stream must be stopped for the OFF pass"
    assert len(rec.calls) == 2


def test_perturbation_ab_fails_when_audio_moves(tmp_path):
    ntr = FakeNTR()
    # ON take at -6 dBFS, OFF take ~2 dB quieter -> beyond the 1 dB tolerance
    rec = FakeRecorder(["good", "good_lo"])
    sess = ConsoleCaptureSession(_cfg(tmp_path), ntr_client=ntr, recorder=rec)
    res = sess.perturbation_ab(passes=1, seconds_per_pass=1.0)
    assert not res.ok
    assert "perturb" in res.message.lower()
    assert res.data["worst_db"] > 1.0


# =========================================================================== #
# ScreenSource seam: per-step opt-in eyes (NTR default + Rosalina screenshot)
# =========================================================================== #
def test_rosalina_screenshot_source_fetches_appeared_bmp_not_litter(tmp_path, monkeypatch):
    _no_deadzone(monkeypatch)
    # /luma/screenshots/ holds litter, including a BOGUS FUTURE-dated file that a
    # newest-by-name pick would wrongly choose. The source must instead fetch the
    # file that APPEARED after the screenshot (set difference), and for "top" the
    # plain framebuffer, not the 3D right-eye.
    ftp = FakeScreenshotFtp({
        "2020-01-01_00-00-00.000_top.bmp": b"old-litter",
        "2031-09-09_09-09-09.999_top.bmp": b"future-litter",  # sorts last by name
    })
    ts = "2026-07-19_10-00-00.123"
    real_top = _make_bmp_bytes((6, 4), (10, 20, 30))
    menu = FakeRosalinaMenu(on_screenshot=lambda: (
        ftp.add(f"{ts}_top.bmp", real_top),
        ftp.add(f"{ts}_top_right.bmp", _make_bmp_bytes((2, 2))),  # 3D on
        ftp.add(f"{ts}_bot.bmp", _make_bmp_bytes((5, 3))),
    ))
    src = _rosa_source(tmp_path, menu, ftp)
    frame = src.grab(NavStep("look", (), menu_leak_safe=True), "top")
    assert frame is not None
    assert frame.size == (6, 4)              # decoded THIS capture's top BMP
    assert menu.screenshots_taken == 1
    assert menu.state == "closed"            # menu opened for the shot, then closed
    # fetched the appeared top framebuffer -- not the 3D right-eye, not the 2031 litter
    assert ftp.gets and ftp.gets[-1].endswith(f"{ts}_top.bmp")
    assert "2031" not in ftp.gets[-1] and "_top_right" not in ftp.gets[-1]


def test_rosalina_screenshot_source_returns_none_when_no_new_bmp(tmp_path, monkeypatch):
    _no_deadzone(monkeypatch)
    # The walk runs but no BMP materialises (e.g. the SD write failed). The source
    # must NOT fall back to the dir's future-dated litter -- it returns None,
    # which the verifier turns into a clean "no frame" checkpoint failure.
    ftp = FakeScreenshotFtp({"2031-01-01_00-00-00.000_top.bmp": b"future-litter"})
    menu = FakeRosalinaMenu()  # no on_screenshot -> nothing appears
    src = _rosa_source(tmp_path, menu, ftp)
    frame = src.grab(NavStep("look", (), menu_leak_safe=True), "top")
    assert frame is None
    assert menu.screenshots_taken == 1   # the walk did activate "Take screenshot"
    assert ftp.gets == []                # nothing fetched -- litter untouched


def test_rosalina_screenshot_source_returns_none_when_baseline_list_fails(tmp_path, monkeypatch):
    _no_deadzone(monkeypatch)
    # If the BASELINE (before) directory listing FAILS transiently, the source must
    # NOT proceed with an empty baseline -- that would make the set difference
    # degrade to the whole directory and fetch the future-dated litter. It bails to
    # None (do-not-guess) before even opening the menu.
    ftp = FakeScreenshotFtp({"2031-01-01_00-00-00.000_top.bmp": b"future-litter"})
    orig_list = ftp.list
    calls = {"n": 0}
    def flaky_list(ip, path="/", port=4952):
        calls["n"] += 1
        if calls["n"] == 1:
            raise RuntimeError("Could not reach the file service")  # baseline fails
        return orig_list(ip, path, port)
    ftp.list = flaky_list
    menu = FakeRosalinaMenu(on_screenshot=lambda: ftp.add(
        "2026-07-19_10-00-00.000_top.bmp", _make_bmp_bytes()))
    src = _rosa_source(tmp_path, menu, ftp)
    frame = src.grab(NavStep("look", (), menu_leak_safe=True), "top")
    assert frame is None
    assert menu.screenshots_taken == 0   # bailed before opening the menu -- no guess
    assert ftp.gets == []                # the 2031 litter is never fetched


def test_rosalina_screenshot_source_refuses_mid_record(tmp_path):
    # Opening the menu freezes the app; the eye must refuse while a take is live.
    ftp = FakeScreenshotFtp({"a_top.bmp": _make_bmp_bytes()})
    menu = FakeRosalinaMenu()
    src = _rosa_source(tmp_path, menu, ftp, is_recording=lambda: True)
    with pytest.raises(RuntimeError, match="mid-record"):
        src.grab(NavStep("look", (), menu_leak_safe=True), "top")
    assert menu.state == "closed"            # the menu was never opened
    assert ftp.gets == []


def test_rosalina_screenshot_source_refuses_leak_unsafe_step(tmp_path):
    # The open combo leaks buttons into the app; a step must vouch it is inert.
    ftp = FakeScreenshotFtp({"a_top.bmp": _make_bmp_bytes()})
    menu = FakeRosalinaMenu()
    src = _rosa_source(tmp_path, menu, ftp)
    with pytest.raises(RuntimeError, match="menu_leak_safe"):
        src.grab(NavStep("look", ()), "top")  # menu_leak_safe defaults to False
    assert menu.state == "closed"
    assert ftp.gets == []


def test_navigate_defaults_to_ntr_source(tmp_path):
    # No step opts out -> every checkpoint uses the NTR eye, exactly as before.
    with SimRig() as rig:
        assert rig.wait_first_frame()
        sess = ConsoleCaptureSession(_cfg(tmp_path), input_client=rig.input, ntr_client=rig.ntr)
        res = sess.navigate(_short_path(2))
        assert res.ok, res.message
        assert all(s.source == "ntr" for s in _short_path(2))


def test_navigate_per_step_rosalina_opt_in(tmp_path, monkeypatch):
    _no_deadzone(monkeypatch)
    with SimRig() as rig:
        assert rig.wait_first_frame()
        ftp = FakeScreenshotFtp({"2030-01-01_00-00-00.000_top.bmp": b"future-litter"})
        ts = "2026-07-19_11-11-11.111"
        menu = FakeRosalinaMenu(
            on_screenshot=lambda: ftp.add(f"{ts}_top.bmp", _make_bmp_bytes((5, 5))))
        rosa = _rosa_source(tmp_path, menu, ftp)
        sess = ConsoleCaptureSession(
            _cfg(tmp_path), input_client=rig.input, ntr_client=rig.ntr,
            screen_sources={"rosalina-screenshot": rosa})
        path = [
            NavStep("ntr-step", (Tap(("A",), hold_ms=5, wait_ms=3),),
                    ExpectScreen("cp0", "top"), settle_ms=3),  # default: NTR
            NavStep("rosa-step", (Tap(("A",), hold_ms=5, wait_ms=3),),
                    ExpectScreen("cp1", "top"), settle_ms=3,
                    source="rosalina-screenshot", menu_leak_safe=True),
        ]
        res = sess.navigate(path)
        assert res.ok, res.message
        assert len(res.data["checkpoints"]) == 2
        # the rosalina step drove the screenshot eye and fetched THIS capture's BMP
        assert ftp.gets and ftp.gets[-1].endswith(f"{ts}_top.bmp")
        assert menu.state == "closed"


def test_navigate_rosalina_step_without_leak_safe_fails_that_checkpoint(tmp_path):
    # A step opting into the screenshot eye but not declaring the leak safe fails
    # its own checkpoint (source error), not crashes the run.
    ftp = FakeScreenshotFtp({"x_top.bmp": _make_bmp_bytes()})
    menu = FakeRosalinaMenu()
    rosa = _rosa_source(tmp_path, menu, ftp)
    with SimRig() as rig:
        assert rig.wait_first_frame()
        sess = ConsoleCaptureSession(
            _cfg(tmp_path), input_client=rig.input, ntr_client=rig.ntr,
            screen_sources={"rosalina-screenshot": rosa})
        path = [NavStep("rosa", (Tap(("A",), hold_ms=5, wait_ms=3),),
                        ExpectScreen("cp", "top"), settle_ms=3,
                        source="rosalina-screenshot")]  # menu_leak_safe False
        res = sess.navigate(path)
        assert not res.ok
        assert res.data["failed_step"] == "rosa"
        assert "menu_leak_safe" in res.data["checkpoints"][0]["note"]


def test_unknown_screen_source_raises(tmp_path):
    sess = ConsoleCaptureSession(_cfg(tmp_path))
    with pytest.raises(ValueError, match="unknown screen source"):
        sess.screen_source("does-not-exist")


# =========================================================================== #
# SoundTestProfile: the plaza-specific navigation contract as a seam
# =========================================================================== #
def test_plaza_profile_reproduces_the_module_path_exactly():
    prof = cc.PLAZA_PROFILE.path_for_track("Main Theme 1")
    mod = plaza_path_for_track("Main Theme 1")
    assert [s.name for s in prof] == [s.name for s in mod]
    assert [s.taps for s in prof] == [s.taps for s in mod]
    assert [s.settle_ms for s in prof] == [s.settle_ms for s in mod]


def test_plaza_profile_composes_the_module_constants():
    assert cc.PLAZA_PROFILE.tracks == cc.PLAZA_TRACKS
    assert cc.PLAZA_PROFILE.seconds_per_pass == cc.SECONDS_PER_PASS_V2
    assert cc.PLAZA_PROFILE.stop_playback_tap == cc.STOP_PLAYBACK_TAP
    assert cc.PLAZA_PROFILE.stop_playback_tap.buttons == ("A",)  # "A stops" (plaza law)
    with pytest.raises(KeyError):
        cc.PLAZA_PROFILE.path_for_track("No Such Track")


def test_config_defaults_to_the_plaza_profile(tmp_path):
    assert _cfg(tmp_path).profile is cc.PLAZA_PROFILE


def test_stop_playback_sends_the_profile_tap(tmp_path):
    with SimRig() as rig:
        sess = ConsoleCaptureSession(_cfg(tmp_path), input_client=rig.input, ntr_client=rig.ntr)
        sess.stop_playback()  # plaza profile -> A
        assert rig.sink.wait_for(2, timeout=2.0)  # press + release
        pressed = [p["pressed"] for p in rig.sink.packets if p["pressed"]]
        assert pressed == [{"A"}]


def test_stop_playback_is_a_noop_when_profile_has_no_stop_tap(tmp_path):
    import time

    prof = cc.SoundTestProfile(name="no-single-tap-stop", tracks={"x": 0},
                               home_to_target_taps=(), seconds_per_pass={},
                               stop_playback_tap=None)
    with SimRig() as rig:
        sess = ConsoleCaptureSession(_cfg(tmp_path, profile=prof),
                                     input_client=rig.input, ntr_client=rig.ntr)
        sess.stop_playback()
        time.sleep(0.15)  # give any stray packet time to (not) arrive
        assert rig.sink.count() == 0  # nothing sent when there is no stop tap
