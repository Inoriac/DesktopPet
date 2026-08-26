import os
import sys
import unittest
from unittest.mock import Mock, patch

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LAUNCHER = os.path.join(ROOT, "launcher")
if LAUNCHER not in sys.path:
    sys.path.insert(0, LAUNCHER)

from process_tracker import PetProcessTracker
from app_state import AppState
from pet_registry import PetEntry
from PySide6.QtWidgets import QApplication
from pages.pet_page import PetPage
from pages.ai_page import AiPage


class FakeProcess:
    def __init__(self, exit_code=None):
        self.exit_code = exit_code

    def poll(self):
        return self.exit_code


class PetProcessTrackerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication(["process-tracker-tests"])

    def test_refresh_whenProcessesExit_shouldKeepOnlyRunningInstances(self):
        tracker = PetProcessTracker()
        first = FakeProcess()
        second = FakeProcess()

        self.assertEqual(tracker.add(first), 1)
        self.assertEqual(tracker.add(second), 2)

        first.exit_code = 0
        self.assertEqual(tracker.refresh(), 1)

        second.exit_code = 1
        self.assertEqual(tracker.refresh(), 0)

    def test_petPage_whenAliveCountChanges_shouldUpdateAdjacentLabel(self):
        profile = PetEntry(
            "Milltina", "assets/models/milltina/model/milltina.gltf",
            "11111111-1111-4111-8111-111111111111")
        with patch("pages.pet_page.load_pets", return_value=[profile]):
            page = PetPage(AppState())

        page.set_alive_count(3)

        self.assertEqual(page.alive_count_label.text(), "运行中：3")

    def test_aiPage_whenSaveButtonClicked_shouldInvokeSaveCallback(self):
        on_save = Mock()
        page = AiPage(AppState(), on_save=on_save)

        page.save_btn.click()

        on_save.assert_called_once_with()

    def test_aiPage_whenViewportIsNarrow_shouldKeepSaveButtonVisible(self):
        page = AiPage(AppState())
        page.resize(760, 720)
        page.show()
        self.app.processEvents()

        button_rect = page.save_btn.rect()
        top_left = page.save_btn.mapTo(page.viewport(), button_rect.topLeft())
        bottom_right = page.save_btn.mapTo(
            page.viewport(), button_rect.bottomRight())

        self.assertGreaterEqual(top_left.x(), 0)
        self.assertLess(bottom_right.x(), page.viewport().width())


if __name__ == "__main__":
    unittest.main()
