import base64
import importlib.util
import pathlib
import subprocess
import sys
import unittest
from unittest import mock

sys.dont_write_bytecode = True
ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("evidence", ROOT / "devices/poco-f4/collect-wifi-ownership.py")
evidence = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(evidence)


class OwnershipEvidenceTests(unittest.TestCase):
    def test_commands_are_read_only_and_fixed(self):
        for name, command in evidence.probes():
            self.assertTrue(command.startswith(("cat ", "readlink -f ", "ls -l", "uname -r", "getprop ")), name)
            for forbidden in ("su ", " root", "reboot", "mount ", "devmem", "/dev/mem", "/resource0", "/config", ">", ";", "\n"):
                self.assertNotIn(forbidden, command)

    def test_matching_identity_is_not_a_grant(self):
        records = {f"endpoint/{name}": {"returncode": 0, "stdout_b64": base64.b64encode(value).decode("ascii")}
                   for name, value in {"vendor": b"0x17cb\n", "device": b"0x1101\n", "revision": b"0x01\n", "class": b"0x028000\n"}.items()}
        assessment = evidence.evaluate(records)
        self.assertTrue(assessment["endpoint_identity_matches"])
        self.assertFalse(assessment["ownership_transferred"])
        self.assertFalse(assessment["safe_to_enable_bus_master"])
        records["endpoint/device"]["returncode"] = 1
        self.assertFalse(evidence.evaluate(records)["endpoint_identity_matches"])

    def test_missing_or_corrupt_identity(self):
        self.assertFalse(evidence.evaluate({})["endpoint_identity_matches"])
        self.assertFalse(evidence.evaluate({"endpoint/vendor": {"returncode": 0, "stdout_b64": "!invalid!"}})["endpoint_identity_matches"])

    @mock.patch.object(evidence.subprocess, "run")
    def test_offline_never_executes_shell(self, run):
        run.return_value = subprocess.CompletedProcess([], 1, b"", b"no device")
        report = evidence.collect("adb", "explicit-serial")
        self.assertFalse(report["connected"])
        self.assertEqual(report["records"], {})
        run.assert_called_once_with(["adb", "-s", "explicit-serial", "get-state"], check=False,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)

    @mock.patch.object(evidence.subprocess, "run")
    def test_only_allowlisted_remote_reads(self, run):
        run.side_effect = lambda arguments, **kwargs: subprocess.CompletedProcess(arguments, 0, b"device\n" if arguments[-1] == "get-state" else b"", b"")
        report = evidence.collect("adb", "selected")
        self.assertTrue(report["connected"])
        allowed = {command for name, command in evidence.probes()}
        for call in run.call_args_list[1:]:
            arguments = call.args[0]
            self.assertEqual(arguments[:4], ["adb", "-s", "selected", "exec-out"])
            self.assertEqual(len(arguments), 5)
            self.assertIn(arguments[4], allowed)

    @mock.patch.object(evidence.subprocess, "run", side_effect=subprocess.TimeoutExpired("adb", 10))
    def test_timeout_is_not_success(self, run):
        report = evidence.collect("adb", "selected")
        self.assertFalse(report["connected"])
        self.assertIsNone(report["connection"]["returncode"])


if __name__ == "__main__":
    unittest.main()
