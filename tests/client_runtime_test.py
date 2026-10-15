"""Filesystem/exit supervision regressions; real client and server are never launched."""
import contextlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/client_runtime"))
import prepare_runtime
import run_runtime
import runtime_support as support


class RuntimeTests(unittest.TestCase):
    def setUp(self):
        (ROOT / "Artifacts/runtime_tests").mkdir(parents=True, exist_ok=True)
        self.root = Path(tempfile.mkdtemp(dir=ROOT / "Artifacts/runtime_tests")).resolve()
        self.source = self.root / "source"
        self.source.mkdir()
        for name, value in {"BubbleFighter.exe": b"packed", "BubbleFighterU.exe": b"unpacked",
                            "login.ini": b"serverAddr=original.invalid\n", "option.ini": b"music=1\n",
                            "engine.dll": b"engine", "excluded.txt": b"keep"}.items():
            (self.source / name).write_bytes(value)
        (self.source / "Data").mkdir()
        (self.source / "Data/a.bin").write_bytes(b"asset")
        self.hashes = {name: support.digest(self.source / name) for name in support.SAMPLES}
        self.target = self.root / "copy"
        self.sample_patch = mock.patch.dict(prepare_runtime.SAMPLES, self.hashes)
        self.sample_patch.start()

    def tearDown(self):
        self.sample_patch.stop()
        # Deletion is confined to the exact temporary test directory we created.
        target = support.owned_path(self.root, ROOT / "Artifacts/runtime_tests")
        shutil.rmtree(target)

    def prepare(self):
        return prepare_runtime.copy_runtime(self.source, self.target, 17110, self.root)

    def test_plan_has_no_filesystem_mutations(self):
        before = {p.relative_to(self.source): p.read_bytes() for p in self.source.rglob("*") if p.is_file()}
        _, target, files = prepare_runtime.plan(self.source, self.target, 17110, self.root)
        self.assertFalse(target.exists())
        self.assertEqual(len(files), 6)
        self.assertEqual(before, {p.relative_to(self.source): p.read_bytes() for p in self.source.rglob("*") if p.is_file()})

    def test_copy_manifest_and_source_preservation(self):
        manifest = self.prepare()
        self.assertEqual(support.verify_runtime(self.target, self.root), manifest)
        self.assertEqual((self.source / "login.ini").read_bytes(), b"serverAddr=original.invalid\n")
        self.assertIn("resolution=1024x768", (self.target / "option.ini").read_text())
        self.assertFalse((self.target / "excluded.txt").exists())
        with self.assertRaises(ValueError): self.prepare()

    def test_distinct_clan_endpoint(self):
        manifest = prepare_runtime.copy_runtime(self.source, self.target, 17110, self.root, clan_port=17111)
        self.assertEqual(support.verify_runtime(self.target, self.root), manifest)
        self.assertIn("clanServerPort=17111", (self.target / "login.ini").read_text())

    def test_bad_clan_endpoint_refused_before_copy(self):
        for port in (17110, 1, -1, 65536, True):
            with self.assertRaises(ValueError):
                prepare_runtime.copy_runtime(self.source, self.target, 17110, self.root, clan_port=port)
            self.assertFalse(self.target.exists())

    def test_legacy_manifest_rejected(self):
        manifest = self.prepare()
        manifest["Version"] = 1
        (self.target / "runtime_manifest.json").write_text(json.dumps(manifest))
        with self.assertRaises(ValueError): support.verify_runtime(self.target, self.root)

    def test_corrupt_resource_is_rejected(self):
        self.prepare()
        (self.target / "Data/a.bin").write_bytes(b"other")
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            support.verify_runtime(self.target, self.root)

    def test_foreign_endpoint_rejected_even_with_updated_hash(self):
        manifest = self.prepare()
        file = self.target / "login.ini"
        file.write_text(file.read_text().replace("127.0.0.1", "8.8.8.8"))
        entry = next(e for e in manifest["Files"] if e["Path"] == "login.ini")
        entry.update(Sha256=support.digest(file), Bytes=file.stat().st_size)
        (self.target / "runtime_manifest.json").write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, "loopback"):
            support.verify_runtime(self.target, self.root)

    def test_traversal_duplicate_and_missing_manifest_entries(self):
        manifest = self.prepare()
        path = self.target / "runtime_manifest.json"
        for name in ("../source/engine.dll", "C:/outside.dll", "engine.dll:stream"):
            bad = json.loads(json.dumps(manifest)); bad["Files"][0]["Path"] = name
            path.write_text(json.dumps(bad))
            with self.assertRaises(ValueError): support.verify_runtime(self.target, self.root)
        bad = json.loads(json.dumps(manifest)); bad["Files"].append(dict(bad["Files"][0]))
        path.write_text(json.dumps(bad))
        with self.assertRaisesRegex(ValueError, "Duplicate"): support.verify_runtime(self.target, self.root)
        manifest["Files"] = [e for e in manifest["Files"] if e["Path"] != "BubbleFighter.exe"]
        path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, "required"): support.verify_runtime(self.target, self.root)

    def test_bad_sample_and_destination_refused_before_copy(self):
        (self.source / "BubbleFighterU.exe").write_bytes(b"wrong")
        with self.assertRaises(ValueError): self.prepare()
        self.assertFalse(self.target.exists())
        for path in (self.root, self.root.parent / "escaped"):
            with self.assertRaises(ValueError): support.owned_path(path, self.root)

    def test_junction_rejected(self):
        with mock.patch.object(Path, "is_junction", return_value=True):
            with self.assertRaisesRegex(ValueError, "Linked"): support.no_links(self.source)

    def test_failed_copy_has_no_launch_manifest(self):
        with mock.patch.object(prepare_runtime.shutil, "copy2", side_effect=OSError("copy failure")):
            with self.assertRaises(OSError): self.prepare()
        self.assertTrue(self.target.exists())
        self.assertFalse((self.target / "runtime_manifest.json").exists())

    def test_ticket_format_and_nested_redaction(self):
        ticket = self.root / "ticket"
        value = "BFS1." + "a" * 64
        ticket.write_text(value, encoding="ascii")
        self.assertEqual(support.read_ticket(ticket), value)
        self.assertNotIn(value, json.dumps(support.redact({"error": [value, "RPC " + value]})))
        for text in (value + "\n", "BFS1." + "z" * 64, value[:-1]):
            ticket.write_text(text, encoding="ascii")
            with self.assertRaises(ValueError): support.read_ticket(ticket)

    def test_preflight_cannot_spawn_or_import_frida(self):
        sample = self.source / "BubbleFighterU.exe"
        with mock.patch.object(run_runtime, "inspect_sample", return_value={}), \
             mock.patch.object(run_runtime, "run", side_effect=AssertionError("Must not run")), \
             mock.patch.object(subprocess, "Popen", side_effect=AssertionError("Must not spawn")), \
             contextlib.redirect_stdout(io.StringIO()) as output:
            run_runtime.main(["--sample", str(sample)])
        result = json.loads(output.getvalue())
        self.assertTrue(result["PreflightOnly"])
        self.assertFalse(result["OriginalClientStarted"])

    def test_unbounded_duration_and_incomplete_run_refused(self):
        for extra in (["--seconds", "0"], ["--seconds", "1201"], ["--run"]):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                run_runtime.arguments(["--sample", str(self.source / "BubbleFighterU.exe"), *extra])

    def test_exit_codes_distinguish_controlled_stop(self):
        support.require_healthy_exit(0, False)
        support.require_healthy_exit(None, True)
        for code in (None, 1, 0xC0000005):
            with self.assertRaises(RuntimeError): support.require_healthy_exit(code, False)

    def test_cleanup_keeps_server_path_when_client_kill_fails(self):
        device, listener = mock.Mock(), mock.Mock()
        device.kill.side_effect = RuntimeError("lost transport")
        listener.poll.return_value = None
        killed, failures = run_runtime.stop_owned(device, 345, threading.Event(), listener)
        self.assertFalse(killed)
        self.assertEqual(failures, ["client:RuntimeError"])
        device.kill.assert_called_once_with(345)
        listener.terminate.assert_called_once()

    def test_cleanup_forces_only_owned_server_after_timeout(self):
        device, listener = mock.Mock(), mock.Mock()
        listener.poll.return_value = None
        listener.wait.side_effect = [subprocess.TimeoutExpired("owned", 10), 0]
        ended = threading.Event(); ended.set()
        killed, failures = run_runtime.stop_owned(device, 345, ended, listener)
        self.assertFalse(killed)
        self.assertEqual(failures, [])
        device.kill.assert_not_called()
        listener.kill.assert_called_once()

    def run_fake(self, configure_failure=False, exit_code=0, detach_reason="process-terminated"):
        fake_project = self.root / "project"
        runtime = fake_project / "Artifacts/client_runtime"
        runtime.mkdir(parents=True)
        ticket = self.root / "private_ticket"
        ticket.write_text("BFS1." + "a" * 64)
        args = run_runtime.arguments(["--run", "--runtime", str(runtime), "--ticket-file", str(ticket),
                                      "--database-config", str(self.root / "db.ini"), "--name", "test"])
        device, session, script, listener, kernel = (mock.Mock() for _ in range(5))
        device.spawn.return_value = 345
        device.attach.return_value = session
        session.create_script.return_value = script
        listener.poll.return_value = None
        kernel.OpenProcess.return_value = 1234
        kernel.GetExitCodeProcess.side_effect = lambda handle, code: (setattr(code._obj, "value", exit_code) or True)
        script.load.side_effect = lambda: script.on.call_args.args[1]({"type": "send", "payload": {"event": "observer-ready"}}, None)
        script.exports_sync.configureclient.return_value = True
        if configure_failure:
            script.exports_sync.configureclient.side_effect = RuntimeError("configure failed")
        device.resume.side_effect = lambda *_: session.on.call_args.args[1](detach_reason)
        device.kill.side_effect = lambda *_: session.on.call_args.args[1]("process-terminated")
        frida = types.SimpleNamespace(get_local_device=lambda: device, __version__="offline-fake")
        with mock.patch.dict(sys.modules, {"frida": frida}), \
             mock.patch.object(run_runtime, "PROJECT", fake_project), \
             mock.patch.object(run_runtime.ctypes, "WinDLL", return_value=kernel), \
             mock.patch.object(run_runtime.subprocess, "Popen", return_value=listener), \
             mock.patch.object(run_runtime.time, "sleep"), \
             mock.patch.object(run_runtime, "owned_path", side_effect=lambda p, root=None: support.owned_path(p, fake_project / "Artifacts")):
            if configure_failure or exit_code != 0 or detach_reason != "process-terminated":
                with self.assertRaises(RuntimeError): run_runtime.run(args, {}, {"Port": 17110})
            else:
                result = run_runtime.run(args, {}, {"Port": 17110})
                self.assertFalse(result["UiAcceptance"])
        listener.terminate.assert_called_once()
        kernel.CloseHandle.assert_called_once_with(1234)
        report = (fake_project / "Artifacts/Logs/client_runtime/test.jsonl").read_text()
        self.assertNotIn(ticket.read_text(), report)
        return device, script, report

    def test_signature_failure_never_resumes_owned_client(self):
        device, _, report = self.run_fake(configure_failure=True)
        device.resume.assert_not_called()
        device.kill.assert_called_once_with(345)
        self.assertIn('"SupervisorStopped": false', report)

    def test_clean_self_exit_is_distinct_from_ui_acceptance(self):
        device, _, report = self.run_fake()
        device.resume.assert_called_once_with(345)
        device.kill.assert_not_called()
        self.assertIn('"ClientExitCode": 0', report)

    def test_crash_exit_is_reported_as_failure(self):
        self.run_fake(exit_code=0xC0000005)

    def test_instrumentation_disconnect_does_not_skip_client_cleanup(self):
        device, _, _ = self.run_fake(detach_reason="connection-terminated")
        device.kill.assert_called_once_with(345)


if __name__ == "__main__":
    unittest.main(verbosity=2)
