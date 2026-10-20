"""QQ package configuration uses temporary profiles and never contacts QQ."""
import sys
sys.dont_write_bytecode = True

import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from plugins.qq import configure


class ConfigureTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="qq config 中文 ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.profile = self.root / "5bb00e6d-937a-4f46-9c87-e3933c078f5a"
        self.profile.mkdir()
        self.host = self.profile / "plugins.json"
        self.settings_path = self.root / "settings.json"
        self.settings = {"backend": "napcat", "wsUrl": "ws://127.0.0.1:3001", "selfId": "999",
                         "connectionKey": "test-account", "stateDir": str(self.root / "state"),
                         "tokenFile": str(self.root / "missing-secret.txt"),
                         "allowedConversations": ["group:123", "private:123"]}
        self.settings_path.write_text(json.dumps(self.settings), encoding="utf-8")

    def plan(self, **kwargs):
        return configure.prepare_configuration(self.profile, self.settings_path, **kwargs)

    def test_preview_no_write_or_token_read(self):
        plan = self.plan()
        self.assertEqual(plan.preview()["mode"], "preview")
        self.assertFalse(self.host.exists())
        self.assertFalse((self.root / "state").exists())
        self.assertFalse(plan.host["observationBindings"][0]["source"]["enabled"])
        self.assertNotIn("autonomy", plan.host)

    def test_precise_grants_identity_and_mappings(self):
        host = self.plan().host
        replies = host["replyBindings"]
        self.assertNotEqual(replies[0]["grantRef"], replies[1]["grantRef"])
        self.assertEqual(host["observationBindings"][0]["selfIdentity"],
                         {"provider": "qq", "namespace": "test-account", "remoteActorId": "999"})
        self.assertEqual(host["configuration"][configure.SOURCE_ID]["script"], str(configure.PACKAGE / "source.py"))
        self.assertEqual(host["resultAdapters"][configure.TOOL_ID + "/send"]["stateMap"]["unknown"], "unknown")
        fields = host["observationAdapters"][0]["fields"]
        self.assertEqual(fields["historicalImport"], "/historical")
        self.assertEqual(fields["speakerDisplayName"], "/display_name")
        for grant in host["grants"]:
            external = grant.get("conversationId") not in (None, "local_owner")
            if external and grant["entryId"] == configure.TOOL_ID:
                self.assertIn(grant["operationId"], ("read", "send", "receipt"))
                self.assertIn(grant["objectArguments"]["/chat_id"], self.settings["allowedConversations"])
                self.assertIsInstance(grant["objectArguments"]["/chat_id"], str)
            elif not external:
                self.assertNotEqual(grant.get("operationId"), "send")
        for reply in replies:
            self.assertEqual(reply["argumentBindings"]["/reply_to"], "replyToRemoteId")

    def test_apply_preserves_unrelated_and_is_idempotent(self):
        old = {"autonomy": {"enabled": False}, "custom": {"keep": 1},
               "plugins": [{"pluginId": "other", "rootPath": "/other", "enabled": False}],
               "grants": [{"id": "other-grant", "pluginId": "other"}],
               "configuration": {"other": {"keep": 2}, configure.TOOL_ID: {"custom": 3}},
               "observationBindings": [{"id": "other-observation", "pluginId": "other"}],
               "observationAdapters": [{"ref": {"pluginId": "other"}}],
               "replyBindings": [{"ref": {"pluginId": "other"}}],
               "resultAdapters": {"other/read": {"dataPointer": "/keep"}}}
        self.host.write_text(json.dumps(old), encoding="utf-8")
        first = configure.apply_configuration(self.plan(source="on"))
        self.assertTrue(first["changed"])
        backup = Path(first["backupDirectory"]) / "plugins.json"
        self.assertEqual(json.loads(backup.read_text(encoding="utf-8")), old)
        self.assertFalse(configure.apply_configuration(self.plan(source="on"))["changed"])
        off = self.plan(autonomous="off", source="on").host
        self.assertEqual(off["autonomy"], old["autonomy"])
        self.assertEqual(off["custom"], old["custom"])
        self.assertEqual(off["configuration"][configure.TOOL_ID]["custom"], 3)
        self.assertEqual(off["configuration"]["other"], {"keep": 2})
        self.assertIn(old["grants"][0], off["grants"])
        self.assertIn(old["replyBindings"][0], off["replyBindings"])
        self.assertFalse(any(g.get("autonomous") for g in off["grants"] if g.get("pluginId") == configure.PLUGIN_ID))
        self.assertTrue(off["observationBindings"][-1]["source"]["enabled"])

    def test_interpreter_keeps_venv_symlink(self):
        python = self.root / "venv" / "bin" / "python"
        python.parent.mkdir(parents=True)
        try:
            python.symlink_to(sys.executable)
        except OSError:
            self.skipTest("Symbolic links unavailable")
        with patch.object(configure.sys, "executable", str(python)):
            host = self.plan().host
        self.assertEqual(host["configuration"][configure.TOOL_ID]["program"], str(python))
        self.assertTrue(all(g.get("executor", str(python)) == str(python) for g in host["grants"]))

    def test_invalid_and_concurrent_config_rejected(self):
        with self.assertRaises(configure.ConfigurationError):
            configure.prepare_configuration(self.root, self.settings_path)
        for value in ("{bad", "[]", '{"observationAdapters":{}}', '{"grants":[1]}'):
            self.host.write_text(value, encoding="utf-8")
            with self.assertRaises(configure.ConfigurationError):
                self.plan()
            self.assertEqual(self.host.read_text(encoding="utf-8"), value)
        self.host.unlink()
        plan = self.plan()
        self.host.write_text('{"changed":true}', encoding="utf-8")
        with self.assertRaises(configure.ConfigurationError):
            configure.apply_configuration(plan)
        self.assertEqual(json.loads(self.host.read_text(encoding="utf-8")), {"changed": True})

    def test_settings_change_after_preview_rejected(self):
        plan = self.plan()
        self.settings["selfId"] = "888"
        self.settings_path.write_text(json.dumps(self.settings), encoding="utf-8")
        with self.assertRaises(configure.ConfigurationError):
            configure.apply_configuration(plan)
        self.assertFalse(self.host.exists())

    def test_binding_id_collision_rejected_and_write_failure_preserves_host(self):
        self.host.write_text(json.dumps({"observationBindings": [{"id": configure.BINDING_ID, "pluginId": "other"}]}), encoding="utf-8")
        with self.assertRaises(configure.ConfigurationError):
            self.plan()
        self.host.write_text('{"custom":1}', encoding="utf-8")
        plan = self.plan()
        write = configure.atomic_write
        def fail_host(path, data):
            if path == self.host:
                raise OSError("disk full")
            write(path, data)
        with patch.object(configure, "atomic_write", side_effect=fail_host), self.assertRaises(configure.ConfigurationError):
            configure.apply_configuration(plan)
        self.assertEqual(self.host.read_text(encoding="utf-8"), '{"custom":1}')

    def test_invalid_settings_no_rooms_or_package_state(self):
        for change in ({"allowedConversations": []}, {"stateDir": str(configure.PACKAGE / "state")}):
            self.settings_path.write_text(json.dumps(dict(self.settings, **change)), encoding="utf-8")
            with self.assertRaises(configure.ConfigurationError):
                self.plan()
        self.assertFalse(self.host.exists())

    def test_cli_preview_utf8_and_package_immutable(self):
        result = subprocess.run([sys.executable, "-B", str(configure.PACKAGE / "configure.py"),
                                 "--profile-dir", str(self.profile), "--settings", str(self.settings_path)], capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout)["mode"], "preview")
        self.assertFalse((configure.PACKAGE / "__pycache__").exists())


if __name__ == "__main__":
    unittest.main()
