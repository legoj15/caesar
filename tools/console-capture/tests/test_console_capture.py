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

    def __init__(self, enabled: bool = False, value: int = 85) -> None:
        self.state = "closed"   # closed | root | sysconfig | volume
        self.cursor = 0
        self.enabled = enabled
        self.value = value
        self.success = False
        self.epoch = 100
        self.press_log: list[frozenset] = []

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


def test_deploy_staging_refusal_gets_actionable_hint(tmp_path):
    class StagingFtp(FakeFtp):
        def put(self, ip, local, remote, port=5000):
            raise RuntimeError("550 Writes are only permitted under /luma/staging/")

    ftp = StagingFtp()
    sess = ConsoleCaptureSession(_cfg(tmp_path), ftp_put=ftp.put, ftp_get=ftp.get,
                                 ftp_list=ftp.list)
    res = sess.deploy(_write_bcsar(tmp_path))
    assert not res.ok
    assert "N3DS_FTP_PORT=5000" in res.message


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
    # HOME opens the menu; DLEFT saturates to the leftmost tile; 3x DRIGHT
    # lands on the plaza tile (live-verified layout, 2026-07-18)
    assert ("HOME",) in all_buttons
    assert sum(1 for b in all_buttons if b == ("DLEFT",)) == 12
    assert sum(1 for b in all_buttons if b == ("DRIGHT",)) == 3
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
