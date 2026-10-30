"""Offline-only tests: no invocation of ADB, Android, or a physical display."""
import contextlib
import errno
import io
import json
import os
import signal
import shlex
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from device_session import BOOT_ID, DISPLAY_LOCK, DRM_CLASS, prune_command, session_script
import run_receiver
import show_hevc


def wait_for(predicate, message, process, timeout=60):
    """Wait for a fixture event; the deadline only detects a hung test."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        if process.poll() is not None:
            raise AssertionError(f"Session exited before {message}: {process.returncode}")
        time.sleep(.02)
    raise AssertionError("Timed out waiting for " + message)


def release_deadline(fifo, process):
    def send():
        try:
            fd = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK)
        except OSError as exc:
            if exc.errno == errno.ENXIO:
                return False
            raise
        with os.fdopen(fd, "w") as file:
            file.write("expired\n")
        return True
    wait_for(send, fifo.name + " reader", process)


class RunnerTest(unittest.TestCase):
    def test_profile_forward_survives_reconnect_and_cleans_invalid_response(self):
        for allocated in ('49152', 'bad'):
            adb = run_receiver.Adb('127.0.0.1:5555')
            with patch.object(adb, 'run') as run, patch('subprocess.run'), contextlib.redirect_stdout(io.StringIO()):
                run.return_value = subprocess.CompletedProcess([], 0, allocated + '\n', '')
                with contextlib.ExitStack() as cleanup:
                    if allocated == 'bad':
                        with self.assertRaisesRegex(RuntimeError, 'allocated profile port'):
                            adb.profile_forward(47801, cleanup)
                    else:
                        self.assertEqual(adb.profile_forward(47801, cleanup), allocated)
                        adb.reconnect()
                        run.assert_called_with('forward', 'tcp:49152', 'tcp:47801', check=False)
                run.assert_called_with('forward', '--remove', 'tcp:' + allocated, check=False)
                self.assertEqual(adb.forwards, {})

    def test_dry_run_executes_nothing(self):
        for mode in ("--no-decode", "--display"):
            with tempfile.TemporaryDirectory() as directory:
                output = Path(directory) / "results with ' quotes"
                text = io.StringIO()
                with contextlib.redirect_stdout(text), patch("subprocess.run") as run:
                    self.assertEqual(run_receiver.main([mode, "--dry-run", "--results", str(output)]), 0)
                    run.assert_not_called()
                self.assertFalse(output.exists())
                plan = text.getvalue()
                self.assertIn("cmake --build", plan)
                self.assertIn("ip -4 addr show dev wlan0", plan)
                self.assertIn("adb connect", plan)
                self.assertIn("receiver-session.sh", plan)
                self.assertIn("run.log", plan)
                script = plan.split("--- BEGIN DEVICE SCRIPT ---\n", 1)[1].split("--- END DEVICE SCRIPT ---", 1)[0]
                subprocess.run(["sh", "-n"], input=script, text=True, check=True)
                if mode == "--display":
                    self.assertNotIn("--display-csv", script)
                    self.assertIn("setprop ctl.restart zygote", script)
                else:
                    for forbidden in ("surfaceflinger", "hwcomposer", "zygote", "/sys/kernel/debug/dri", "getprop"):
                        self.assertNotIn(forbidden, script)

    def test_addresses(self):
        output = "    inet 127.0.0.1/8 scope host lo\n    inet 192.168.1.24/24 scope global wlan0\n"
        self.assertEqual(run_receiver.box_addresses(output), ["192.168.1.24"])

    def test_local_dry_run_never_uses_adb_or_creates_files(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "local with ' quotes"
            text = io.StringIO()
            with contextlib.redirect_stdout(text), patch("subprocess.run") as run, \
                    patch("subprocess.Popen") as popen, patch.object(run_receiver, "Adb") as adb:
                self.assertEqual(run_receiver.main(["--local", "--dump", "--profile", "--dry-run",
                                                    "--seconds", "30",
                                                    "--results", str(output)]), 0)
                run.assert_not_called()
                popen.assert_not_called()
                adb.assert_not_called()
            self.assertFalse(output.exists())
            plan = text.getvalue()
            self.assertIn("build/host", plan)
            self.assertIn("--no-decode", plan)
            self.assertIn("stream.hevc", plan)
            self.assertIn("--host 192.168.185.100", plan)
            self.assertIn("Run vmtest.bat 192.168.185.100", plan)
            self.assertNotIn("--pace-mbps", plan)
            for text in ("adb", "surfaceflinger", "zygote", "/sys/kernel/debug/dri"):
                self.assertNotIn(text, plan.lower())

    def test_local_rejects_display(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
            run_receiver.parse_args(["--local", "--display"])
        self.assertEqual(error.exception.code, 2)

    def test_local_and_box_duration_limits(self):
        for mode in ("--local", "--no-decode", "--display"):
            self.assertEqual(run_receiver.parse_args([mode]).seconds, 0)
            self.assertEqual(run_receiver.parse_args([mode, "--seconds", "0"]).seconds, 0)
        self.assertEqual(run_receiver.parse_args(["--local", "--seconds", "3600"]).seconds, 3600)
        for args in (["--local", "--seconds", "-1"], ["--no-decode", "--seconds", "86401"]):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                run_receiver.parse_args(args)

    def test_unlimited_session_has_no_lifetime_watchdog(self):
        with tempfile.TemporaryDirectory() as directory:
            remote = Path(directory)
            script = session_script(remote, ["sh", "-c", "sleep 0.1; echo finished"], 0)
            # Even watchdog 0 would KILL a live child after 3 seconds, so the
            # unlimited path must not start it. Cleanup still has its guard.
            self.assertNotIn("watchdog ", script.split("lls_child=$!", 1)[1])
            result = subprocess.run(["sh"], input=script, text=True, capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("finished", result.stdout)
            self.assertEqual((remote / "status").read_text().strip(), "0")
            self.assertFalse((remote / "watchdog-expired").exists())

    def test_show_uses_device_restoration_result_without_host_polling(self):
        for restored, display_check in (("1", "required: HDMI connected"),
                                        ("1", "not checked: no HDMI connector reports connected"),
                                        ("0", "required: HDMI connected")):
            with self.subTest(restored=restored, display_check=display_check), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                clip = root / "test.hevc"
                clip.touch()
                artifacts = {"display.log": "display finished\n", "restoration-status": restored + "\n",
                             "restoration-display-check.txt": display_check + "\n",
                             "restored-display.txt": "ACTIVE 1920x1080p60\n" if display_check.startswith("required:") else "INACTIVE\n",
                             "timings.csv": "timings\n"}

                def run(command, **kwargs):
                    output = ""
                    if command[0] == "cmake":
                        pass
                    elif command[3] == "shell":
                        shell = command[4]
                        self.assertNotIn("getprop", shell)
                        self.assertNotIn("/sys/kernel/debug/dri", shell)
                        if shell == "id -u":
                            output = "0\n"
                        elif "/active-display.txt" in shell:
                            output = "ACTIVE 1920x1080p60 NV12\n"
                        elif "/status" in shell:
                            output = "0\n"
                    elif command[3] == "pull":
                        Path(command[5]).write_text(artifacts[Path(command[4]).name])
                    return subprocess.CompletedProcess(command, 0, output, "")

                with patch.object(show_hevc, "ROOT", root), patch("subprocess.run", side_effect=run), \
                        patch("time.sleep"), contextlib.redirect_stdout(io.StringIO()):
                    if restored == "1":
                        show_hevc.main(["--input", str(clip)])
                    else:
                        with self.assertRaisesRegex(RuntimeError, "did not confirm Android restoration"):
                            show_hevc.main(["--input", str(clip)])
                results = next((root / "build/results").iterdir())
                self.assertEqual((results / "restoration-status").read_text().strip(), restored)

    def test_packet_trace_is_opt_in_for_probe(self):
        text = io.StringIO()
        with contextlib.redirect_stdout(text), patch("subprocess.run") as run:
            self.assertEqual(run_receiver.main(["--no-decode", "--profile", "--dump", "--frames", "60", "--dry-run"]), 0)
            run.assert_not_called()
        self.assertIn("--profile 47801", text.getvalue())
        self.assertIn("forward tcp:0 tcp:47801", text.getvalue())
        self.assertIn("profile.py", text.getvalue())
        self.assertIn("--dump", text.getvalue())
        self.assertIn("/stream.hevc", text.getvalue())
        self.assertIn("--frames 60", text.getvalue())

    def test_diagnostics_require_limits_and_display_options_require_display(self):
        for extra in (["--dump"], ["--dump", "--seconds", "0"],
                      ["--frames", "-1"], ["--plane-color-space", "3"]):
            with self.subTest(extra=extra), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                run_receiver.parse_args(["--local", *extra])

    def test_prune_preserves_active_symlink_unknown_and_unpulled_runs(self):
        with tempfile.TemporaryDirectory() as directory:
            parent = Path(directory) / "sessions with ' quotes"
            parent.mkdir()
            for i in range(10):
                run = parent / f"20261027T0000{i:02d}.0Z"
                run.mkdir()
                if i != 1:
                    (run / "status").write_text("0")
                if i == 2:
                    (run / "stream.hevc").write_bytes(b"unpulled")
            (parent / "unknown").mkdir()
            (parent / "20261026T000000.0Z").symlink_to(parent / "20261027T000009.0Z", target_is_directory=True)
            subprocess.run(prune_command(parent), check=True)
            self.assertFalse((parent / "20261027T000000.0Z").exists())
            self.assertTrue((parent / "20261027T000001.0Z").is_dir())
            self.assertTrue((parent / "20261027T000002.0Z/stream.hevc").is_file())
            self.assertTrue((parent / "20261026T000000.0Z").is_symlink())
            self.assertTrue((parent / "unknown").is_dir())
            for i in range(5, 10):
                self.assertTrue((parent / f"20261027T0000{i:02d}.0Z").is_dir())

    def test_collection_retries_transport_errors(self):
        adb = Mock()
        adb.shell.side_effect = [subprocess.CompletedProcess([], 1, "", "device offline"),
                                 subprocess.CompletedProcess([], 0, "present\n", ""),
                                 subprocess.CompletedProcess([], 0, "missing\n", "")]
        with patch("time.sleep"):
            missing = run_receiver.collect(adb, "/device", Path("/local"), ["rx.csv", "packets.csv"],
                                           deadline=time.monotonic() + 10)
        self.assertEqual(missing, ["packets.csv"])
        adb.reconnect.assert_called_once()
        adb.run.assert_called_once_with("pull", "/device/rx.csv", Path("/local/rx.csv"))

    def test_dump_removed_only_after_verified_pull(self):
        for remote_size in (5, 6):
            with tempfile.TemporaryDirectory() as directory:
                results = Path(directory)
                adb = Mock()
                def pull(*args):
                    (results / "stream.hevc").write_bytes(b"video")
                adb.run.side_effect = pull
                adb.shell.side_effect = [subprocess.CompletedProcess([], 0, "present\n", ""),
                                         subprocess.CompletedProcess([], 0, str(remote_size) + "\n", ""),
                                         subprocess.CompletedProcess([], 0, "", "")]
                if remote_size == 5:
                    self.assertEqual(run_receiver.collect(adb, "/device", results, ["stream.hevc"], deadline=0), [])
                    adb.shell.assert_called_with("rm", "-f", "/device/stream.hevc")
                else:
                    with self.assertRaisesRegex(RuntimeError, "size mismatch"):
                        run_receiver.collect(adb, "/device", results, ["stream.hevc"], deadline=0)
                    self.assertFalse(any(call.args[0] == "rm" for call in adb.shell.call_args_list))

    def test_verified_dump_cleanup_retries_without_pulling_again(self):
        with tempfile.TemporaryDirectory() as directory:
            results = Path(directory)
            (results / "stream.hevc").write_bytes(b"video")
            adb = Mock()
            adb.shell.side_effect = [subprocess.CompletedProcess([], 0, "present\n", ""),
                                     subprocess.CompletedProcess([], 0, "5\n", ""),
                                     subprocess.CalledProcessError(1, "rm", stderr="device offline"),
                                     subprocess.CompletedProcess([], 0, "", "")]
            with patch("time.sleep"):
                self.assertEqual(run_receiver.collect(adb, "/device", results, ["stream.hevc"],
                                                       deadline=time.monotonic() + 10), [])
            adb.run.assert_called_once()
            adb.shell.assert_called_with("rm", "-f", "/device/stream.hevc")
            adb.reconnect.assert_called_once()

    def run_script(self, *, display, states=None, status=0, hang=False, locked=False,
                   stubborn=None, signal_session=False, stale_boot=False, stale_pid=False,
                   connected=True, active=True, apk=False, framework=False):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            remote = base / "session with ' quotes"
            remote.mkdir()
            fake = base / "bin"
            fake.mkdir()
            properties = {"init.svc." + name: value for name, value in (states or {}).items()}
            properties["init.svc.bootanim"] = "stopped"
            state = base / "properties.json"
            state.write_text(json.dumps(properties))
            trace = base / "mutations.txt"
            trace.write_text("")
            helper = ("#!" + sys.executable + "\n" + '''
import json, os, pathlib, sys
name = pathlib.Path(sys.argv[0]).name
path = pathlib.Path(os.environ['LLS_MOCK_STATE'])
data = json.loads(path.read_text())
if name == 'pidof':
    print(os.environ.get('LLS_MOCK_SYSTEM_SERVER', ''))
elif name == 'getprop':
    print(data.get(sys.argv[1], 'stopped'))
else:
    framework = os.environ.get('LLS_MOCK_SYSTEM_SERVER')
    if framework and ((name == 'stop' and sys.argv[-1] == 'surfaceflinger') or name == 'setprop'):
        paused = pathlib.Path('/proc/' + framework + '/stat').read_text().split()[2] == 'T'
        if paused != (name == 'stop'):
            with open(os.environ['LLS_MOCK_TRACE'], 'a') as file:
                file.write('ERROR framework pause/restore order\\n')
    if name == 'start' and os.environ.get('LLS_MOCK_PROGRAM_PID'):
        pid = pathlib.Path(os.environ['LLS_MOCK_PROGRAM_PID'])
        if pid.exists():
            try:
                os.kill(int(pid.read_text()), 0)
            except ProcessLookupError:
                pass
            else:
                with open(os.environ['LLS_MOCK_TRACE'], 'a') as file:
                    file.write('ERROR service started while receiver alive\\n')
    with open(os.environ['LLS_MOCK_TRACE'], 'a') as file:
        file.write(name + ' ' + ' '.join(sys.argv[1:]) + '\\n')
    service = sys.argv[-1]
    data['init.svc.' + service] = 'stopped' if name == 'stop' else 'running'
    path.write_text(json.dumps(data))
''')
            for name in ("getprop", "start", "stop", "setprop", "pidof"):
                (fake / name).write_text(helper)
                (fake / name).chmod(0o755)
            # Control the two watchdog deadlines explicitly. CPU scheduling
            # must never turn an ordinary exit into a watchdog expiry, or kill
            # a TERM-handling fixture before this test has observed its state.
            # Only this fake sleep changes; the generated product script and
            # its three-second TERM->KILL grace remain untouched.
            expiry, grace = base / "expiry.fifo", base / "grace.fifo"
            os.mkfifo(expiry)
            os.mkfifo(grace)
            gate = base / "gate.py"
            gate.write_text('''
import os, pathlib, select, signal, sys
signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))
fd = os.open(sys.argv[1], os.O_RDWR | os.O_NONBLOCK)
pathlib.Path(sys.argv[2]).touch()
while True:
    select.select([fd], [], [])
    if os.read(fd, 128):
        break
''')
            gate_command = shlex.join([sys.executable, str(gate)])
            sleep_body = '''case "$1" in
61) exec GATE "$LLS_MOCK_EXPIRY" "$LLS_MOCK_WATCHDOG_READY" ;;
3) exec GATE "$LLS_MOCK_GRACE" "$LLS_MOCK_GRACE_READY" ;;
0.02) while [ ! -f input-ready ]; do /bin/sleep .02; done ;;
*) exec /bin/sleep .005 ;;
esac'''.replace('GATE', gate_command)
            (fake / "sleep").write_text("#!/bin/sh\n" + sleep_body + "\n")
            (fake / "sleep").chmod(0o755)
            debug = base / "dri"
            debug.mkdir()
            (debug / "summary").write_text(("ACTIVE" if active else "INACTIVE") + " 1920x1080p60 NV12 color_space[3]\n")
            (debug / "state").write_text("mock DRM state\n")
            connectors = base / "drm"
            (connectors / "card0-HDMI-A-1").mkdir(parents=True)
            (connectors / "card0-HDMI-A-1/status").write_text("connected\n" if connected else "disconnected\n")
            lock = base / "lock"
            boot_id = base / "boot_id"
            boot_id.write_text("current-boot\n")
            if locked or stale_boot or stale_pid:
                lock.mkdir()
                (lock / "boot_id").write_text("previous-boot\n" if stale_boot else boot_id.read_text())
                # A reaped child's pid is gone; this test's pid is definitely live.
                dead = subprocess.Popen(["sh", "-c", "exit 0"])
                dead.wait()
                (lock / "pid").write_text(str(dead.pid if stale_pid else os.getpid()) + "\n")
            # Finish an ordinary receiver only after the fake timer has set
            # its signal handler. Otherwise cancellation can beat exec under
            # load and leave an artificial infinite FIFO wait behind.
            command = ["sh", "-c", ("" if apk else
                       'while [ ! -f "$LLS_MOCK_WATCHDOG_READY" ]; do /bin/sleep .005; done; ') +
                       "exit " + str(status)]
            pidfile = base / "program.pid"
            if hang or stubborn is not None:
                program = base / "program.py"
                program.write_text('''
import os, pathlib, signal, sys, time
pid, term, release = map(pathlib.Path, sys.argv[1:4])
stubborn = sys.argv[4] == 'True'
def stop(signum, frame):
    term.write_text('TERM received')
    if not stubborn:
        while not pathlib.Path(os.environ['LLS_MOCK_GRACE_READY']).exists():
            time.sleep(.005)
        sys.exit(0)
signal.signal(signal.SIGTERM, stop)
pid.write_text(str(os.getpid()))
while not release.exists():
    time.sleep(.01)
''')
                command = [sys.executable, str(program), str(pidfile), str(base / "term"),
                           str(base / "release-program"), str(stubborn is not None)]
            watchdog = 61  # distinctive fixture deadline, advanced through expiry.fifo
            if apk:
                receiver = remote / "libllcast_rx.so"
                receiver.write_text("#!/bin/sh\nexec " + shlex.join(command) + "\n")
                receiver.chmod(0o755)
                helper = remote / "libllcast_session.so"
                helper.write_text("#!" + sys.executable + "\n" + '''
import os, pathlib, signal, sys
assert sys.argv[1] == '--watch-back' and int(sys.argv[2]) == os.getppid()
signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
pathlib.Path('input.pid').write_text(str(os.getpid()))
pathlib.Path('input-ready').touch()
while True:
    signal.pause()
''')
                helper.chmod(0o755)
            script = session_script(remote, [] if apk else command, 0 if apk else watchdog, display=display, apk=apk)
            # Redirect only hardware filesystem paths; command structure stays identical.
            script = script.replace(DISPLAY_LOCK, str(lock)).replace(BOOT_ID, str(boot_id))
            script = script.replace("/sys/kernel/debug/dri/0", str(debug))
            script = script.replace(DRM_CLASS, str(connectors))
            subprocess.run(["sh", "-n"], input=script, text=True, check=True)
            script_file = base / "session.sh"
            script_file.write_text(script)
            env = dict(os.environ, PATH=str(fake) + os.pathsep + os.environ["PATH"],
                       LLS_MOCK_STATE=str(state), LLS_MOCK_TRACE=str(trace), LLS_MOCK_PROGRAM_PID=str(pidfile),
                       LLS_MOCK_EXPIRY=str(expiry), LLS_MOCK_GRACE=str(grace),
                       LLS_MOCK_WATCHDOG_READY=str(base / "watchdog-ready"),
                       LLS_MOCK_GRACE_READY=str(base / "grace-ready"))
            system_server = subprocess.Popen([sys.executable, "-c", "import signal; signal.pause()"])
            if framework:
                env["LLS_MOCK_SYSTEM_SERVER"] = str(system_server.pid)
            launch = ["sh", str(script_file)]
            if apk:
                launch = ["sh", "-c", "exec 5>/dev/null; exec " +
                          shlex.join(launch + [str(remote), str(receiver), "/dev/dri/card0", str(helper)])]
            process = subprocess.Popen(launch, env=env, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, text=True, start_new_session=True)
            try:
                if hang or stubborn is not None:
                    wait_for(lambda: pidfile.exists() and pidfile.stat().st_size > 0, "receiver ready", process)
                if (hang or stubborn is not None) and not apk:
                    wait_for((base / "watchdog-ready").exists, "watchdog ready", process)
                if signal_session:
                    process.send_signal(signal.SIGTERM)
                    wait_for((base / "term").exists, "receiver TERM handler", process)
                    self.assertIsNone(process.poll(), "Must wait for the stubborn receiver")
                    wait_for((base / "grace-ready").exists, "cleanup grace ready", process)
                    process.send_signal(signal.SIGTERM)  # Cleanup must ignore further signals.
                    (base / "release-program").touch()
                elif hang or stubborn is not None:
                    release_deadline(expiry, process)
                    if stubborn is not None:
                        wait_for((base / "term").exists, "receiver TERM handler", process)
                        self.assertIsNone(process.poll(), "Must wait until the KILL grace expires")
                        release_deadline(grace, process)
                stdout, stderr = process.communicate(timeout=90)
            finally:
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.communicate(timeout=5)
                framework_state = Path(f"/proc/{system_server.pid}/stat").read_text().split()[2]
                system_server.send_signal(signal.SIGCONT)
                system_server.terminate()
                system_server.wait(timeout=5)
                if pidfile.exists():
                    pid = int(pidfile.read_text())
                    try:
                        os.kill(pid, 0)
                    except ProcessLookupError:
                        pass
                    else:
                        os.kill(pid, signal.SIGKILL)
                        self.fail("Receiver outlived the restoration session")
            result = subprocess.CompletedProcess(process.args, process.returncode, stdout, stderr)
            expected = 75 if locked else 143 if signal_session else 124 if hang or stubborn is not None else status
            restored = not (display and states and states.get("surfaceflinger") == "running" and connected and not active)
            if not restored and expected == 0:
                expected = 70
            self.assertEqual(int((remote / "status").read_text()), expected, result.stdout + result.stderr)
            if not locked:
                self.assertEqual(int((remote / "program-status").read_text()), 0 if expected == 70 else expected)
                if display:
                    self.assertEqual((remote / "restoration-status").read_text().strip(), str(int(restored)))
                    self.assertTrue((remote / "before-display.txt").exists())
                    self.assertTrue((remote / "restored-display.txt").exists())
                    self.assertFalse(lock.exists())
                    if apk:
                        with self.assertRaises(ProcessLookupError):
                            os.kill(int((remote / "input.pid").read_text()), 0)
                    if not connected and states and states.get("surfaceflinger") == "running":
                        self.assertIn("not checked", (remote / "restoration-display-check.txt").read_text())
            self.assertNotIn("ERROR service started while receiver alive", trace.read_text())
            self.assertNotIn("ERROR framework", trace.read_text())
            self.assertNotEqual(framework_state, "T", "The guard left system_server suspended")
            if stubborn is not None:
                self.assertTrue((base / "term").exists(), "Watchdog/session TERM must reach the program")
            if stale_boot or stale_pid:
                self.assertIn("Removing stale display lock", result.stderr)
            return trace.read_text().splitlines(), json.loads(state.read_text())

    def test_restore_success_failure_and_watchdog(self):
        states = {name: "running" for name in ("surfaceflinger", "vendor.hwcomposer-2-1", "zygote", "zygote_secondary")}
        expected = ["stop surfaceflinger", "stop vendor.hwcomposer-2-1", "start vendor.hwcomposer-2-1",
                    "start surfaceflinger", "setprop ctl.restart zygote_secondary", "setprop ctl.restart zygote"]
        for status, hang in ((0, False), (7, False), (0, True)):
            trace, after = self.run_script(display=True, states=states, status=status, hang=hang, framework=True)
            self.assertEqual(trace, expected)
            self.assertTrue(all(after["init.svc." + name] == "running" for name in states))

    def test_preserve_preexisting_stopped_services(self):
        trace, _ = self.run_script(display=True, states={"surfaceflinger": "stopped", "vendor.hwcomposer-2-1": "running"})
        self.assertEqual(trace, ["stop vendor.hwcomposer-2-1", "start vendor.hwcomposer-2-1"])
        trace, _ = self.run_script(display=True, states={"surfaceflinger": "running", "zygote": "running"})
        self.assertEqual(trace, ["stop surfaceflinger", "start surfaceflinger", "setprop ctl.restart zygote"])

    def test_apk_embedded_guard_reaps_input_and_restores_after_receiver_crash(self):
        states = {name: "running" for name in ("surfaceflinger", "vendor.hwcomposer-2-1", "zygote")}
        trace, after = self.run_script(display=True, states=states, status=7, apk=True, framework=True)
        self.assertEqual(trace, ["stop surfaceflinger", "stop vendor.hwcomposer-2-1",
                               "start vendor.hwcomposer-2-1", "start surfaceflinger", "setprop ctl.restart zygote"])
        self.assertTrue(all(after["init.svc." + name] == "running" for name in states))

    def test_watchdog_kills_and_reaps_program_before_restoring_services(self):
        self.run_script(display=True, states={"surfaceflinger": "running"}, stubborn=0)

    def test_session_term_waits_for_program_and_ignores_repeated_term(self):
        self.run_script(display=True, states={"surfaceflinger": "running"}, stubborn=.3, signal_session=True, framework=True)

    def test_reclaim_lock_from_old_boot_or_dead_pid(self):
        self.run_script(display=True, stale_boot=True)  # PID is live/reused, but boot differs.
        self.run_script(display=True, stale_pid=True)

    def test_projector_off_skips_only_the_scanout_check(self):
        self.run_script(display=True, states={"surfaceflinger": "running"}, connected=False, active=False)
        self.run_script(display=True, states={"surfaceflinger": "running"}, connected=True, active=False)

    def test_no_decode_and_busy_display_touch_no_services(self):
        trace, _ = self.run_script(display=False)
        self.assertEqual(trace, [])
        trace, _ = self.run_script(display=True, locked=True)
        self.assertEqual(trace, [])


if __name__ == "__main__":
    unittest.main()
