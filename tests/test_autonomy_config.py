import os
import sys
import unittest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(__file__)), "launcher"))
from PySide6.QtWidgets import QApplication
from qfluentwidgets import SwitchButton
from app_state import AppState
from config_loader import apply_settings
from pages.ai_page import AiPage


class AutonomyConfigTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication(["autonomy-config-tests"])

    def test_default_off_explicit_opt_in_and_single_switch_round_trip(self):
        state = AppState()
        self.assertFalse(state.autonomy_enabled)
        self.assertFalse(hasattr(state, "autonomy_paused"))
        state.autonomy_enabled = True
        source = {
            "aiSettings": {
                "autonomy": {
                    "paused": True,
                    "maxTokensPerHour": 45678,
                    "memoryAccess": {"owner": True, "conversationIds": ["room"]},
                },
                "activeProfile": "one",
                "profiles": {"one": {"enabled": False}},
            }
        }
        saved = apply_settings(source, state.to_settings_dict())
        self.assertEqual(saved["aiSettings"]["autonomy"]["maxTokensPerHour"], 45678)
        self.assertEqual(
            saved["aiSettings"]["autonomy"]["memoryAccess"],
            {"owner": True, "conversationIds": ["room"]},
        )
        self.assertTrue(saved["aiSettings"]["autonomy"]["enabled"])
        self.assertFalse(saved["aiSettings"]["autonomy"]["paused"])

        restored = AppState.from_config(saved)
        self.assertTrue(restored.autonomy_enabled)

        # Legacy configs with paused=True normalize to autonomy_enabled=False
        # and clear paused when re-exported.
        legacy_paused = {
            "aiSettings": {
                "autonomy": {"enabled": True, "paused": True},
            }
        }
        from_paused = AppState.from_config(legacy_paused)
        self.assertFalse(from_paused.autonomy_enabled)
        reexported = apply_settings(legacy_paused, from_paused.to_settings_dict())
        self.assertFalse(reexported["aiSettings"]["autonomy"]["enabled"])
        self.assertFalse(reexported["aiSettings"]["autonomy"]["paused"])
        self.assertFalse(source["aiSettings"]["profiles"]["one"]["enabled"])

    def test_ai_page_exposes_single_autonomy_switch(self):
        state = AppState(autonomy_enabled=False)
        page = AiPage(state)
        switches = page.findChildren(SwitchButton)
        # AI 开关, 自主活动, 情绪系统, 自动屏幕聊天
        self.assertEqual(len(switches), 4)
        page.autonomy_switch.setChecked(True)
        self.assertTrue(state.autonomy_enabled)
        page.autonomy_switch.setChecked(False)
        self.assertFalse(state.autonomy_enabled)


if __name__ == "__main__":
    unittest.main()
