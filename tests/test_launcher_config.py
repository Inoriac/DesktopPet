import copy
import json
import os
import sys
import tempfile
import unittest


REPOSITORY_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPOSITORY_ROOT, "launcher"))

import config_loader  # noqa: E402
from app_state import AppState  # noqa: E402


class LauncherEmotionConfigTests(unittest.TestCase):
    def test_app_state_exports_emotion_setting(self):
        state = AppState(emotion_enabled=False)
        settings = state.to_settings_dict()
        self.assertIn("emotionEnabled", settings)
        self.assertFalse(settings["emotionEnabled"])

    def test_apply_settings_updates_active_profile_only(self):
        template = {
            "aiSettings": {
                "activeProfile": "selected",
                "profiles": {
                    "default": {"emotion": {"enabled": True}},
                    "selected": {"emotion": {"enabled": True}},
                },
            }
        }
        output = config_loader.apply_settings(template, {"emotionEnabled": False})
        profiles = output["aiSettings"]["profiles"]
        self.assertTrue(profiles["default"]["emotion"]["enabled"])
        self.assertFalse(profiles["selected"]["emotion"]["enabled"])
        self.assertTrue(template["aiSettings"]["profiles"]["selected"]["emotion"]["enabled"])

    def test_template_round_trip_keeps_emotion_block(self):
        template = config_loader.load_template()
        output = config_loader.apply_settings(
            copy.deepcopy(template), {"emotionEnabled": False})
        profile = output["aiSettings"]["profiles"][output["aiSettings"]["activeProfile"]]
        self.assertFalse(profile["emotion"]["enabled"])
        self.assertEqual(profile["emotion"]["decay"]["valenceHalfLifeSec"], 3600)

        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "config.json")
            with open(path, "w", encoding="utf-8") as handle:
                json.dump(output, handle, ensure_ascii=False)
            with open(path, "r", encoding="utf-8") as handle:
                loaded = json.load(handle)
        self.assertFalse(
            loaded["aiSettings"]["profiles"]["default"]["emotion"]["enabled"])


if __name__ == "__main__":
    unittest.main()
