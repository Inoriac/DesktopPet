"""Explicit setup and manual login, with network and package installation replaced."""
import asyncio
import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from plugins.bilibili import setup, login


class SetupTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="bili setup 中文 ")
        self.addCleanup(self.temp.cleanup)
        self.state = Path(self.temp.name).resolve() / "state"

    def test_base_setup_does_not_install_asr_or_download_model(self):
        with patch.object(setup.sys, "version_info", (3, 13)), patch.object(setup.subprocess, "run") as run:
            result = setup.prepare(self.state)
        calls = [call.args[0] for call in run.call_args_list]
        self.assertEqual(len(calls), 2)
        self.assertIn("venv", calls[0])
        self.assertTrue(calls[1][-1].endswith("requirements.txt"))
        self.assertNotIn("asrModelPath", result)
        self.assertTrue(result["python"].startswith(str(self.state)))
        self.assertFalse(any("download_model" in part for call in calls for part in call))

    def test_explicit_asr_prepares_selected_model_in_state(self):
        with patch.object(setup.sys, "version_info", (3, 13)), patch.object(setup.subprocess, "run") as run:
            result = setup.prepare(self.state, "tiny")
        calls = [call.args[0] for call in run.call_args_list]
        self.assertEqual(len(calls), 4)
        self.assertTrue(calls[2][-1].endswith("requirements-asr.txt"))
        self.assertEqual(calls[3][-2:], ["tiny", str(self.state / "models" / "tiny")])
        self.assertEqual(result["asrModelPath"], str(self.state / "models" / "tiny"))
        self.assertEqual(run.call_args.kwargs["env"]["HF_HOME"], str(self.state / "hf-cache"))

    def test_setup_rejects_old_python_and_package_state_before_writing(self):
        with patch.object(setup.sys, "version_info", (3, 9)), self.assertRaises(setup.SetupError):
            setup.prepare(self.state)
        self.assertFalse(self.state.exists())
        with patch.object(setup.sys, "version_info", (3, 13)), self.assertRaises(setup.SetupError):
            setup.prepare(setup.PACKAGE / "state")

    def test_setup_failure_is_actionable_without_raw_download_url(self):
        with patch.object(setup.sys, "version_info", (3, 13)), patch.object(setup.subprocess, "run", side_effect=subprocess.CalledProcessError(1, "secret")):
            with self.assertRaises(setup.SetupError) as raised:
                setup.prepare(self.state)
        self.assertNotIn("secret", str(raised.exception))
        self.assertIn("setup", str(raised.exception).lower())

    def test_interpreter_symlink_is_preserved(self):
        python = str(Path(self.temp.name) / "venv" / "bin" / "python")
        with patch.object(setup.sys, "version_info", (3, 13)), patch.object(setup.sys, "executable", python), patch.object(setup.subprocess, "run") as run:
            setup.prepare(self.state)
        self.assertEqual(run.call_args_list[0].args[0][0], python)


class FakeQr:
    def __init__(self, states=("SCAN", "CONF", "DONE")):
        self.states = iter(states)
    async def generate_qrcode(self):
        pass
    async def check_state(self):
        return SimpleNamespace(name=next(self.states))
    def get_qrcode_picture(self):
        return SimpleNamespace(content=b"controlled QR png")
    def get_credential(self):
        return SimpleNamespace(sessdata="SECRET", bili_jct="TOKEN", buvid3="b3", buvid4=None,
                               dedeuserid="42", ac_time_value="TIME", extra_secret="DO_NOT_COPY")


class LoginTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="bili login 中文 ")
        self.addCleanup(self.temp.cleanup)
        self.state = Path(self.temp.name).resolve() / "state"

    def test_success_saves_private_credentials_without_printing(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            result = asyncio.run(login.login(self.state, qr=FakeQr(), poll_seconds=0))
        saved = self.state / "credential.json"
        data = json.loads(saved.read_text(encoding="utf-8"))
        self.assertEqual(data["SESSDATA"], "SECRET")
        self.assertEqual(data["DedeUserID"], "42")
        self.assertNotIn("extra_secret", data)
        self.assertNotIn("SECRET", output.getvalue() + json.dumps(result))
        self.assertFalse((self.state / "login-qr.png").exists())
        if os.name != "nt":
            self.assertEqual(saved.stat().st_mode & 0o777, 0o600)
            self.assertEqual(self.state.stat().st_mode & 0o777, 0o700)

    def test_expired_qr_preserves_previous_credential_and_removes_image(self):
        self.state.mkdir()
        saved = self.state / "credential.json"
        saved.write_text('{"SESSDATA":"old"}', encoding="utf-8")
        with self.assertRaises(login.LoginError):
            asyncio.run(login.login(self.state, qr=FakeQr(("TIMEOUT",)), poll_seconds=0))
        self.assertEqual(json.loads(saved.read_text(encoding="utf-8"))["SESSDATA"], "old")
        self.assertFalse((self.state / "login-qr.png").exists())

    def test_network_timeout_leaves_no_partial_credential(self):
        qr = FakeQr()
        async def hang():
            await asyncio.sleep(1)
        qr.generate_qrcode = hang
        with self.assertRaises(login.LoginError):
            asyncio.run(login.login(self.state, qr=qr, timeout_seconds=0.01))
        self.assertFalse((self.state / "credential.json").exists())

    def test_login_cli_normalizes_legacy_console_encoding(self):
        source = ("import sys; sys.dont_write_bytecode=True; "
                  "from plugins.bilibili import login\n"
                  "async def local_login(path):\n print('二维码: ' + path)\n return {}\n"
                  "login.login=local_login\n"
                  "sys.argv=['login','--state-dir',sys.argv[1]]\n"
                  "sys.exit(login.main())\n")
        result = subprocess.run([sys.executable, "-B", "-c", source, str(self.state)],
                                cwd=str(Path(__file__).resolve().parents[1]), capture_output=True,
                                env=dict(os.environ, PYTHONIOENCODING="cp1252"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("二维码", result.stdout.decode("utf-8"))


if __name__ == "__main__":
    unittest.main()
