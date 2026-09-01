import os
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LAUNCHER = os.path.join(ROOT, "launcher")
if LAUNCHER not in sys.path:
    sys.path.insert(0, LAUNCHER)

from process_tracker import PetProcessTracker
from app_state import AppState, ModelEndpointState, ModelRoleState
from pet_registry import PetEntry, PetRegistryError
from PySide6.QtWidgets import QApplication
import main
from main import LauncherWindow
from pages.pet_page import PetPage
from pages.ai_page import AiPage
from pages.advanced_page import AdvancedPage
from pages.bubble_page import BubblePage
from qfluentwidgets import CompactSpinBox, ToolButton


class FakeProcess:
    def __init__(self, exit_code=None, wait_times_out=False):
        self.exit_code = exit_code
        self.wait_times_out = wait_times_out
        self.terminate_calls = 0
        self.kill_calls = 0

    def poll(self):
        return self.exit_code

    def terminate(self):
        self.terminate_calls += 1

    def wait(self, timeout=None):
        if self.wait_times_out and self.kill_calls == 0:
            raise subprocess.TimeoutExpired("Desktop_Pet", timeout)
        self.exit_code = -9 if self.kill_calls else 0
        return self.exit_code

    def kill(self):
        self.kill_calls += 1


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

    def test_terminateAll_whenProcessDoesNotExit_shouldForceKillTrackedOnly(self):
        tracker = PetProcessTracker()
        graceful = FakeProcess()
        stubborn = FakeProcess(wait_times_out=True)
        tracker.add(graceful)
        tracker.add(stubborn)

        tracker.terminate_all(timeout_seconds=0)

        self.assertEqual(graceful.terminate_calls, 1)
        self.assertEqual(graceful.kill_calls, 0)
        self.assertEqual(stubborn.terminate_calls, 1)
        self.assertEqual(stubborn.kill_calls, 1)
        self.assertEqual(tracker.refresh(), 0)

    def test_launcher_whenCoreExitsDuringStartup_shouldReportFailure(self):
        launcher = Mock()
        launcher._refresh_alive_pet_count.return_value = 0
        process = FakeProcess(exit_code=2)

        with patch("main.InfoBar.error") as show_error:
            LauncherWindow._confirm_core_started(
                launcher, process, "Milltina")

        launcher.pet_page.set_starting.assert_called_once_with(False)
        launcher._close_owner_diary_session.assert_called_once_with()
        launcher._close_chat_session.assert_called_once_with()
        show_error.assert_called_once()

    def test_resolveCore_whenNewerDebugIsIncompatible_shouldPreferCompatibleRelease(self):
        with tempfile.TemporaryDirectory() as directory:
            release = os.path.join(
                directory, "cmake-build-release-mingw_qt", "Desktop_Pet.exe")
            debug = os.path.join(
                directory, "cmake-build-debug-mingw_qt", "Desktop_Pet.exe")
            os.makedirs(os.path.dirname(release))
            os.makedirs(os.path.dirname(debug))
            with open(release, "wb") as executable:
                executable.write(b"binary launcher-chat-bootstrap protocol")
            with open(debug, "wb") as executable:
                executable.write(b"old binary")
            os.utime(debug, (2_000_000_000, 2_000_000_000))

            with patch.object(main, "_PROJECT_ROOT", directory), \
                    patch.object(main, "_EXE_SUFFIX", ".exe"), \
                    patch.dict(os.environ, {}, clear=False):
                os.environ.pop("DESKTOP_PET_EXECUTABLE", None)
                selected = main.resolve_cpp_executable()

            self.assertEqual(selected, release)

    def test_petPage_whenAliveCountChanges_shouldUpdateAdjacentLabel(self):
        profile = PetEntry(
            "Milltina", "assets/models/milltina/model/milltina.gltf",
            "11111111-1111-4111-8111-111111111111")
        with patch("pages.pet_page.load_pets", return_value=[profile]):
            page = PetPage(AppState())

        page.set_alive_count(3)

        self.assertEqual(page.alive_count_label.text(), "运行中：3")
        self.assertIsInstance(page.size_spin, CompactSpinBox)
        self.assertEqual(page.size_spin.width(), 132)

    def test_petPage_whileCoreIsStarting_shouldDisableStartButton(self):
        profile = PetEntry(
            "Milltina", "assets/models/milltina/model/milltina.gltf",
            "11111111-1111-4111-8111-111111111111")
        start = Mock(return_value=True)
        with patch("pages.pet_page.load_pets", return_value=[profile]):
            page = PetPage(AppState(), on_start=start)

        page.start_btn.click()

        start.assert_called_once_with()
        self.assertEqual(page.start_btn.text(), "启动中")
        self.assertFalse(page.start_btn.isEnabled())
        page.set_starting(False)
        self.assertEqual(page.start_btn.text(), "启动桌宠")
        self.assertTrue(page.start_btn.isEnabled())

    def test_petPage_whenRegistryIsInvalid_shouldRemainUsable(self):
        state = AppState()
        with patch(
                "pages.pet_page.load_pets",
                side_effect=PetRegistryError("invalid registry")):
            page = PetPage(state)

        self.assertEqual(page.registry_error, "invalid registry")
        self.assertFalse(page.start_btn.isEnabled())
        self.assertEqual(state.pet_name, "")

    def test_aiPage_whenSaveButtonClicked_shouldInvokeSaveCallback(self):
        on_save = Mock()
        page = AiPage(AppState(), on_save=on_save)

        page.save_btn.click()

        on_save.assert_called_once_with()

    def test_aiPage_whenEndpointModelChanges_shouldUpdateReferencedRoles(self):
        state = AppState(
            model_endpoints={
                "DEFAULT": ModelEndpointState(),
                "VISION_VENDOR": ModelEndpointState(),
            },
            model_roles={
                "dialogue": ModelRoleState("DEFAULT", "old-model"),
                "vision": ModelRoleState("VISION_VENDOR", "vision-model"),
            },
        )
        page = AiPage(state)

        page.endpoint_model.setText("new-default-model")

        self.assertEqual(
            state.model_roles["dialogue"].model, "new-default-model")
        self.assertEqual(state.model_roles["vision"].model, "vision-model")
        self.assertEqual(
            page._role_model_edits["dialogue"].text(), "new-default-model")

    def test_aiPage_whenConnectionWorksButAiIsOff_shouldExplainDisabledState(self):
        page = AiPage(AppState(ai_enabled=False))

        with patch("pages.ai_page.InfoBar.warning") as show_warning:
            page._show_connection_test_result(
                "DEFAULT", True, "success", "连接成功")

        show_warning.assert_called_once()
        self.assertIn("AI 开关尚未启用", show_warning.call_args.args[1])

    def test_launcherPages_shouldUseCompactNumericAndIconControls(self):
        page = AiPage(AppState())
        bubble_spin = BubblePage._spin(1, 200, 14, lambda _value: None)
        advanced_wrap = AdvancedPage._spin(
            -2000, 2000, -20, lambda _value: None)

        self.assertIsInstance(page.min_spin, CompactSpinBox)
        self.assertIsInstance(page.max_spin, CompactSpinBox)
        self.assertIsInstance(bubble_spin, CompactSpinBox)
        self.assertEqual(
            len(advanced_wrap.findChildren(CompactSpinBox)), 1)
        self.assertIsInstance(page.delete_endpoint_button, ToolButton)
        self.assertIsInstance(page.add_endpoint_button, ToolButton)

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
