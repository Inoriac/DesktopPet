"""The music configurator only writes the explicitly selected temporary profile."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


PACKAGE = Path(__file__).resolve().parents[1] / "plugins" / "lx-music"
SCRIPT = PACKAGE / "configure.py"
PROFILE_ID = "12345678-1234-4234-8234-123456789abc"
PLUGIN = "desktop-pet.lx-music"
TOOL = PLUGIN + ".tools"
SKILL = PLUGIN + ".skill"
READ = {"status", "lyric", "list_playlists", "playlist_songs"}


class LxMusicConfigureTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="lx configure ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.profile = self.root / "profiles" / PROFILE_ID
        self.profile.mkdir(parents=True)
        self.database = self.root / "未安装 music.db"
        self.host = self.profile / "plugins.json"
        self.settings = self.profile / "lx-music.settings.json"

    def run_configure(self, *args, success=True):
        result = subprocess.run([sys.executable, str(SCRIPT), "--profile-dir", str(self.profile),
                                 "--database", str(self.database), *args],
                                capture_output=True, timeout=10)
        if success:
            self.assertEqual(result.returncode, 0, result.stderr.decode("utf-8"))
        else:
            self.assertNotEqual(result.returncode, 0)
        return json.loads(result.stdout)

    def import_configure(self):
        spec = importlib.util.spec_from_file_location("lx_configure", SCRIPT)
        module = importlib.util.module_from_spec(spec)
        sys.path.insert(0, str(PACKAGE))
        self.addCleanup(sys.path.remove, str(PACKAGE))
        spec.loader.exec_module(module)
        return module

    def write_host(self, value):
        self.host.write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")

    def create_symlink(self, link, target, directory=False):
        try:
            link.symlink_to(target, target_is_directory=directory)
        except OSError as error:
            if os.name == "nt" and getattr(error, "winerror", None) == 1314:
                self.skipTest("Windows account cannot create symbolic links")
            raise

    def test_configuration_preview_no_write(self):
        result = self.run_configure()
        self.assertEqual(result["mode"], "preview")
        self.assertEqual(list(self.profile.iterdir()), [])
        self.assertFalse(self.database.exists())
        self.assertEqual(result["profileId"], PROFILE_ID)
        self.assertEqual(result["hostConfigurationPath"], str(self.host))
        host = result["hostConfiguration"]
        self.assertEqual(host["plugins"][0]["rootPath"], str(PACKAGE))
        self.assertEqual(host["configuration"][TOOL]["program"], str(Path(sys.executable).resolve()))
        self.assertEqual(host["configuration"][TOOL]["settings"], str(self.settings))
        self.assertEqual(result["connectionSettings"]["database"], str(self.database))
        self.assertEqual(len(host["resultAdapters"]), 12)
        for adapter in host["resultAdapters"].values():
            self.assertEqual(adapter["dataPointer"], "/data")
            self.assertEqual(adapter["statePointer"], "/state")
            self.assertEqual(adapter["stateMap"]["unknown"], "unknown")
            self.assertNotIn("errorPointer", adapter)

    def test_apply_preserves_unrelated_and_backup(self):
        other = self.root / "other"
        other.mkdir()
        (other / "plugin.json").write_text('{"id":"other.plugin"}', encoding="utf-8")
        unrelated_grant = {"id": "custom", "pluginId": "other.plugin", "profileId": PROFILE_ID,
                           "entryId": "other.tool", "conversationId": "local_owner"}
        before = {"custom": {"nested": [1, "你好"]}, "plugins": [
            {"rootPath": str(other), "enabled": False, "vendor": "keep"}],
            "configuration": {"other.tool": {"setting": 42}, TOOL: {"extra": "keep"}},
            "grants": [unrelated_grant], "connections": {"server": {"program": "/keep"}},
            "resultAdapters": {"other.tool/read": {"dataPointer": "/value"}}}
        self.write_host(before)
        self.settings.write_text('{"database":"/old.db"}', encoding="utf-8")
        originals = {path.name: path.read_bytes() for path in (self.host, self.settings)}
        result = self.run_configure("--apply")
        after = json.loads(self.host.read_text(encoding="utf-8"))
        self.assertEqual(after["custom"], before["custom"])
        self.assertEqual(after["plugins"][0], before["plugins"][0])
        self.assertEqual(after["configuration"]["other.tool"], before["configuration"]["other.tool"])
        self.assertEqual(after["configuration"][TOOL]["extra"], "keep")
        self.assertEqual(after["grants"][0], unrelated_grant)
        self.assertEqual(after["connections"], before["connections"])
        self.assertEqual(after["resultAdapters"]["other.tool/read"], before["resultAdapters"]["other.tool/read"])
        for filename, content in originals.items():
            self.assertEqual((Path(result["backupDirectory"]) / filename).read_bytes(), content)

    def test_apply_idempotent(self):
        first = self.run_configure("--apply")
        before = (self.host.read_bytes(), self.settings.read_bytes())
        second = self.run_configure("--apply")
        self.assertTrue(first["changed"])
        self.assertFalse(second["changed"])
        self.assertIsNone(second["backupDirectory"])
        self.assertEqual(before, (self.host.read_bytes(), self.settings.read_bytes()))
        host = json.loads(self.host.read_text(encoding="utf-8"))
        self.assertEqual(len(host["plugins"]), 1)
        self.assertEqual(len(host["grants"]), 13)
        self.assertEqual(len({grant["id"] for grant in host["grants"]}), 13)

    def test_autonomy_off_read_all(self):
        for mode, expected in (("off", set()), ("read", READ), ("all", None)):
            result = self.run_configure("--autonomous", mode)
            grants = result["hostConfiguration"]["grants"]
            local = [grant for grant in grants if grant.get("conversationId") == "local_owner"]
            self.assertEqual(len(local), 13)
            self.assertTrue(all(not grant["autonomous"] for grant in local))
            internal = [grant for grant in grants if grant.get("scope") == "internal_activity"]
            self.assertTrue(all(grant["autonomous"] and "conversationId" not in grant for grant in internal))
            tool_ops = {grant["operationId"] for grant in internal if grant["entryId"] == TOOL}
            self.assertEqual(tool_ops, expected if expected is not None else {
                grant["operationId"] for grant in local if grant["entryId"] == TOOL})
            self.assertEqual(len([grant for grant in internal if grant["entryId"] == SKILL]), int(mode != "off"))
            if mode == "read":
                self.assertTrue(all(grant["effects"] == ["read"] for grant in internal))

    def test_invalid_configuration_unchanged(self):
        for invalid in ("{bad", '[]', '{"plugins":{}}', '{"grants":{}}',
                        '{"configuration":[]}', '{"resultAdapters":[]}',
                        '{"plugins":[false]}', '{"plugins":[],"plugins":[]}',
                        '{"extra":NaN}', '{"configuration":{"other":false}}'):
            with self.subTest(invalid=invalid):
                self.host.write_text(invalid, encoding="utf-8")
                self.run_configure("--apply", success=False)
                self.assertEqual(self.host.read_text(encoding="utf-8"), invalid)
                self.assertFalse(self.settings.exists())

    def test_invalid_profile_and_api_unchanged(self):
        for profile in (self.root / "absent", self.root / "not-a-uuid",
                        self.root / PROFILE_ID.upper()):
            if profile.name != "absent":
                profile.mkdir()
            result = subprocess.run([sys.executable, str(SCRIPT), "--profile-dir", str(profile),
                                     "--database", str(self.database), "--apply"], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
        self.run_configure("--api-url", "http://example.com", "--apply", success=False)
        self.assertEqual(list(self.profile.iterdir()), [])

    def test_legacy_entry_identity_and_collision(self):
        self.write_host({"plugins": [{"rootPath": str(PACKAGE), "enabled": False, "note": "keep"}]})
        result = self.run_configure("--apply")
        self.assertEqual(len(result["hostConfiguration"]["plugins"]), 1)
        entry = result["hostConfiguration"]["plugins"][0]
        self.assertEqual(entry["pluginId"], PLUGIN)
        self.assertEqual(entry["note"], "keep")
        self.assertTrue(entry["enabled"])
        for entries in ([{"rootPath": str(PACKAGE), "pluginId": "wrong"}],
                        [{"rootPath": str(self.root / "missing")}],
                        [{"rootPath": str(PACKAGE)}, {"rootPath": str(PACKAGE)}]):
            self.write_host({"plugins": entries})
            before = self.host.read_bytes()
            self.run_configure("--apply", success=False)
            self.assertEqual(self.host.read_bytes(), before)

    def test_grant_conflicts_and_revocation(self):
        all_config = self.run_configure("--apply", "--autonomous", "all")["hostConfiguration"]
        result = self.run_configure("--apply")
        self.assertFalse(any(grant.get("scope") for grant in result["hostConfiguration"]["grants"]))
        collision = dict(all_config["grants"][0], pluginId="other.plugin", entryId="other.tool")
        self.write_host({"grants": [collision]})
        before = self.host.read_bytes()
        self.run_configure("--apply", success=False)
        self.assertEqual(before, self.host.read_bytes())

    def test_apply_failure_rollback(self):
        module = self.import_configure()
        for existing in (False, True):
            with self.subTest(existing=existing):
                for path in (self.host, self.settings):
                    if path.exists():
                        path.unlink()
                if existing:
                    self.write_host({"other": "untouched"})
                    self.settings.write_text('{"database":"/old.db"}', encoding="utf-8")
                before = {path: path.read_bytes() if path.exists() else None for path in (self.host, self.settings)}
                plan = module.prepare_configuration(self.profile, self.database)
                real_replace = os.replace
                failed = [False]

                def fail_second(source, target):
                    if Path(target) == self.host and not failed[0]:
                        failed[0] = True
                        raise OSError("injected replace failure")
                    return real_replace(source, target)

                with patch.object(module.os, "replace", side_effect=fail_second):
                    with self.assertRaises(module.ConfigurationError):
                        module.apply_configuration(plan)
                self.assertTrue(failed[0])
                for path, original in before.items():
                    self.assertEqual(path.read_bytes() if path.exists() else None, original)
                self.assertFalse(list(self.profile.glob("*.tmp")))

    def test_apply_detects_changed_original_and_symlinks(self):
        module = self.import_configure()
        plan = module.prepare_configuration(self.profile, self.database)
        self.write_host({"new": "concurrent edit"})
        with self.assertRaises(module.ConfigurationError):
            module.apply_configuration(plan)
        self.assertFalse(self.settings.exists())
        self.host.unlink()
        external = self.root / "outside.json"
        external.write_text("{}", encoding="utf-8")
        self.create_symlink(self.host, external)
        self.run_configure("--apply", success=False)
        self.assertEqual(external.read_text(encoding="utf-8"), "{}")

    def test_preserves_other_plugin_conditional_grant(self):
        permission = {"id": "other-condition", "profileId": PROFILE_ID,
                      "pluginId": "other.plugin", "entryId": "other.tool",
                      "conversationId": "local_owner", "effects": ["conditional"]}
        self.write_host({"grants": [permission]})
        result = self.run_configure("--apply")
        self.assertEqual(result["hostConfiguration"]["grants"][0], permission)

    def test_backup_symlink_cannot_write_outside_profile(self):
        outside = self.root / "outside-backups"
        outside.mkdir()
        self.create_symlink(self.profile / "lx-music-backups", outside, directory=True)
        self.run_configure("--apply", success=False)
        self.assertEqual(list(outside.iterdir()), [])
        self.assertFalse(self.host.exists())
        self.assertFalse(self.settings.exists())

    def test_copied_package_configures_without_repository(self):
        copied = self.root / "独立 music package"
        shutil.copytree(PACKAGE, copied, ignore=shutil.ignore_patterns("__pycache__"))
        result = subprocess.run([sys.executable, str(copied / "configure.py"),
                                 "--profile-dir", str(self.profile), "--database", str(self.database)],
                                cwd=str(self.root), capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout.decode("utf-8"))
        config = json.loads(result.stdout)["hostConfiguration"]
        self.assertEqual(config["plugins"][0]["rootPath"], str(copied))
        self.assertEqual(config["configuration"][TOOL]["script"], str(copied / "adapter.py"))
        self.assertEqual(list(self.profile.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
