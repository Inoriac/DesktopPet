import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(__file__)), "launcher"))
from app_state import AppState
from config_loader import apply_settings


class AutonomyConfigTests(unittest.TestCase):
    def test_default_off_explicit_opt_in_and_pause_round_trip(self):
        state = AppState()
        self.assertFalse(state.autonomy_enabled)
        self.assertFalse(state.autonomy_paused)
        state.autonomy_enabled = True
        state.autonomy_paused = True
        source = {"aiSettings": {"autonomy": {"maxTokensPerHour": 45678},
                  "activeProfile": "one", "profiles": {"one": {"enabled": False}}}}
        saved = apply_settings(source, state.to_settings_dict())
        self.assertEqual(saved["aiSettings"]["autonomy"]["maxTokensPerHour"], 45678)
        restored = AppState.from_config(saved)
        self.assertTrue(restored.autonomy_enabled)
        self.assertTrue(restored.autonomy_paused)
        restored.autonomy_paused = False
        saved = apply_settings(saved, restored.to_settings_dict())
        self.assertFalse(AppState.from_config(saved).autonomy_paused)
        self.assertFalse(source["aiSettings"]["profiles"]["one"]["enabled"])


if __name__ == "__main__":
    unittest.main()
