import copy
import json
import os
import sys
import tempfile
import unittest
from unittest.mock import patch


REPOSITORY_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPOSITORY_ROOT, "launcher"))

import config_loader  # noqa: E402
from app_state import AppState  # noqa: E402


class LauncherConfigTests(unittest.TestCase):
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

    def test_saved_config_restores_api_and_launcher_fields(self):
        state = AppState(
            ai_enabled=True,
            auto_screen_chat=True,
            provider="openai-compatible",
            base_url="https://provider.example/v1",
            api_key="saved-secret",
            model="text-model",
            visual_model="vision-model",
            scale_percent=135,
            snap_enabled=False,
        )
        config = config_loader.apply_settings(
            config_loader.load_template(), state.to_settings_dict())
        overrides = state.to_advanced_overrides()
        config["renderSettings"].update(overrides["renderSettings"])
        config["interactionSettings"].update({
            "dragThreshold": overrides["interactionSettings"]["dragThreshold"],
            "clickTimeout": overrides["interactionSettings"]["clickTimeout"],
        })
        config["interactionSettings"]["windowSnapping"].update(
            overrides["interactionSettings"]["windowSnapping"])

        restored = AppState.from_config(config)

        self.assertTrue(restored.ai_enabled)
        self.assertTrue(restored.auto_screen_chat)
        self.assertEqual(restored.base_url, "https://provider.example/v1")
        self.assertEqual(restored.api_key, "saved-secret")
        self.assertEqual(restored.model, "text-model")
        self.assertEqual(restored.visual_model, "vision-model")
        self.assertEqual(restored.scale_percent, 135)
        self.assertFalse(restored.snap_enabled)

    def test_export_then_load_saved_config_round_trips_json(self):
        config = {"aiSettings": {"activeProfile": "default"}}
        with tempfile.TemporaryDirectory() as directory, patch.object(
                config_loader, "_app_data_dir", return_value=directory):
            path = config_loader.export_config(config)
            restored = config_loader.load_saved_config()

        self.assertEqual(path, os.path.join(directory, "launch_config.json"))
        self.assertEqual(restored, config)


if __name__ == "__main__":
    unittest.main()
