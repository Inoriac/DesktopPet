"""Single-role host configuration with no dependency installs or user profile reads."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from plugins.bilibili import configure


class ConfigureTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="bili config 中文 ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.profile = self.root / "5bb00e6d-937a-4f46-9c87-e3933c078f5a"
        self.profile.mkdir()
        self.state = self.root / "state"
        self.host = self.profile / "plugins.json"

    def plan(self, **kwargs):
        return configure.prepare_configuration(self.profile, self.state, **kwargs)

    def test_preview_does_not_write_and_default_allows_autonomous_reading(self):
        plan = self.plan()
        self.assertEqual(plan.preview()["mode"], "preview")
        self.assertFalse(self.host.exists())
        self.assertFalse(self.state.exists())
        grants = plan.host["grants"]
        self.assertTrue(any(g["entryId"].endswith(".skill") and g.get("scope") == "internal_activity" for g in grants))
        self.assertFalse(any(g.get("operationId") == "start_transcription" for g in grants))
        self.assertTrue(all(g.get("scope") == "internal_activity" or g.get("conversationId") == "local_owner" for g in grants))
        self.assertNotIn("autonomy", plan.host)

    def test_merge_idempotent_backup_and_off_revoke_only_this_internal_grants(self):
        old = {"custom": {"retain": 7}, "autonomy": {"enabled": False},
               "plugins": [{"pluginId": "other.plugin", "rootPath": str(self.root / "other"), "enabled": False}],
               "grants": [{"id": "other", "pluginId": "other.plugin", "entryId": "other.skill", "profileId": self.profile.name,
                           "scope": "internal_activity", "autonomous": True}],
               "configuration": {"other.tool": {"keep": "yes"}},
               "resultAdapters": {"other.tool/read": {"dataPointer": "/keep"}}}
        self.host.write_text(json.dumps(old), encoding="utf-8")
        first = configure.apply_configuration(self.plan())
        self.assertTrue(first["changed"])
        self.assertEqual(json.loads((Path(first["backupDirectory"]) / "plugins.json").read_text(encoding="utf-8")), old)
        self.assertFalse(configure.apply_configuration(self.plan())["changed"])
        off = self.plan(autonomous="off")
        self.assertEqual(off.host["custom"], old["custom"])
        self.assertEqual(off.host["autonomy"], old["autonomy"])
        self.assertEqual(off.host["configuration"]["other.tool"], {"keep": "yes"})
        self.assertTrue(any(g["id"] == "other" for g in off.host["grants"]))
        self.assertFalse(any(g["pluginId"] == configure.PLUGIN_ID and "scope" in g for g in off.host["grants"]))

    def test_asr_explicit_enabling_grants_start_and_real_receipt_mapping(self):
        model = self.root / "model"
        model.mkdir()
        for name in ("model.bin", "config.json", "tokenizer.json"):
            (model / name).write_text("{}", encoding="utf-8")
        plan = self.plan(asr_model_path=model)
        self.assertTrue(plan.settings["asrEnabled"])
        start = [g for g in plan.host["grants"] if g.get("operationId") == "start_transcription"]
        self.assertEqual(len(start), 2)
        self.assertTrue(all(g["effects"] == ["write"] for g in start))
        mappings = plan.host["resultAdapters"]
        mapping = mappings[configure.TOOL_ID + "/start_transcription"]
        self.assertEqual(mapping["receiptPointer"], "/receipt")
        self.assertEqual(mapping["trackingPointer"], "/tracking")
        self.assertEqual(mapping["stateMap"]["accepted"], "accepted")
        for key, value in mappings.items():
            if not key.endswith("/start_transcription"):
                self.assertNotIn("accepted", value["stateMap"])

    def test_interpreter_keeps_virtual_environment_symlink(self):
        python = self.root / "env" / "bin" / "python"
        python.parent.mkdir(parents=True)
        try:
            python.symlink_to(sys.executable)
        except OSError:
            self.skipTest("Creating symbolic links is unavailable for this account")
        with patch.object(configure.sys, "executable", str(python)):
            plan = self.plan()
        self.assertEqual(plan.host["configuration"][configure.TOOL_ID]["program"], str(python))
        self.assertTrue(all(g.get("executor", str(python)) == str(python) for g in plan.host["grants"]))

    def test_bad_profile_json_or_model_does_not_mutate(self):
        with self.assertRaises(configure.ConfigurationError):
            configure.prepare_configuration(self.root, self.state)
        self.host.write_text("{broken", encoding="utf-8")
        with self.assertRaises(configure.ConfigurationError):
            self.plan()
        self.assertEqual(self.host.read_text(encoding="utf-8"), "{broken")
        self.host.unlink()
        with self.assertRaises(configure.ConfigurationError):
            self.plan(asr_model_path=self.root / "missing")

    def test_apply_backup_failure_preserves_both_original_files(self):
        configure.apply_configuration(self.plan())
        plan = self.plan(autonomous="off")
        originals = {p: p.read_bytes() for p in plan.originals}
        with patch.object(configure.Path, "mkdir", side_effect=OSError("full disk")):
            with self.assertRaises(configure.ConfigurationError):
                configure.apply_configuration(plan)
        self.assertEqual({p: p.read_bytes() for p in plan.originals}, originals)

    def test_second_replacement_failure_rolls_back_first_and_new_files(self):
        plan = self.plan()
        write = configure.atomic_write
        def fail_host(path, data):
            if path == self.host:
                raise OSError("denied")
            write(path, data)
        with patch.object(configure, "atomic_write", side_effect=fail_host), self.assertRaises(configure.ConfigurationError):
            configure.apply_configuration(plan)
        self.assertFalse(self.host.exists())
        self.assertFalse(plan.settings_path.exists())

    def test_concurrent_profile_change_aborts_apply(self):
        plan = self.plan()
        self.host.write_text('{"changed":true}', encoding="utf-8")
        with self.assertRaises(configure.ConfigurationError):
            configure.apply_configuration(plan)
        self.assertEqual(json.loads(self.host.read_text(encoding="utf-8")), {"changed": True})

    def test_cli_preview_is_utf8_and_does_not_make_bytecode(self):
        result = subprocess.run([sys.executable, "-B", str(configure.PACKAGE / "configure.py"),
                                 "--profile-dir", str(self.profile), "--state-dir", str(self.state)], capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout)["mode"], "preview")
        self.assertFalse((configure.PACKAGE / "__pycache__").exists())


if __name__ == "__main__":
    unittest.main()
