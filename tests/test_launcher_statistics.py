"""Read-only runtime overview regression tests using synthetic profiles and IPC."""

from datetime import datetime, timedelta, timezone
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch, Mock
from types import SimpleNamespace

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "launcher"))

from PySide6.QtCore import QObject, Signal
from PySide6.QtWidgets import QApplication, QStackedWidget, QWidget
from qfluentwidgets import Theme, setTheme

from statistics_data import load_saved_statistics, profile_directory
from pages.statistics_page import StatisticsPage
from launcher_chat_client import LauncherChatClient
import main

PROFILE = "11111111-1111-4111-8111-111111111111"
OTHER_PROFILE = "22222222-2222-4222-8222-222222222222"


def sample_snapshot(profile=PROFILE, now=None):
    now = now or datetime.now(timezone.utc)
    counters = dict(callCount=12, successCount=9, failureCount=2, cancelledCount=1,
                    unknownUsageCount=2, promptTokens=42000, completionTokens=8000,
                    totalTokens=50000, reasoningTokens=1200, cachedTokens=6000,
                    promptCacheHitTokens=6000, promptCacheMissTokens=36000)
    models = [
        {**counters, "role": "dialogue", "provider": "demo-provider", "model": "demo-model",
         "callCount": 42, "successCount": 35, "failureCount": 3, "cancelledCount": 4,
         "unknownUsageCount": 5, "totalTokens": 150000, "promptTokens": 125000,
         "completionTokens": 25000, "reasoningTokens": 1000, "cachedTokens": 5000,
         "promptCacheHitTokens": 5000, "promptCacheMissTokens": 120000},
        {**counters, "role": "diary", "provider": "demo-provider", "model": "demo-small",
         "callCount": 42, "successCount": 36, "failureCount": 6, "cancelledCount": 0,
         "unknownUsageCount": 5, "totalTokens": 32000, "promptTokens": 27000,
         "completionTokens": 5000, "reasoningTokens": 200, "cachedTokens": 1000,
         "promptCacheHitTokens": 1000, "promptCacheMissTokens": 26000},
    ]
    daily = []
    for i, total in enumerate((2000, 8000, 15000, 22000, 28000, 40000, 50000)):
        count = (1, 4, 6, 8, 10, 11, 12)[i]
        daily.append({"date": (now.astimezone().date()-timedelta(days=6-i)).isoformat(),
                      **dict.fromkeys(counters, 0), "callCount": count, "successCount": count,
                      "promptTokens": total*84//100, "completionTokens": total*16//100,
                      "totalTokens": total})
    daily[-1].update(counters)
    recent = []
    for i in range(63):
        success, cancelled = i % 5 != 0, i % 10 == 0
        recent.append(dict(attemptId=f"synthetic-{i}",
                           completedAt=(now - timedelta(hours=i * 4)).isoformat(),
                           role="dialogue" if i % 2 == 0 else "diary",
                           provider="demo-provider", model="demo-model" if i % 2 == 0 else "demo-small",
                           routeId="demo-route", success=success, cancelled=cancelled,
                           result="cancelled" if cancelled else "success" if success else "failure",
                           durationMs=1250 + i, usageReported=i % 7 != 0,
                           usage={"promptTokens": 700, "completionTokens": 90, "totalTokens": 790}))
    return {
        "schemaVersion": 1, "profileId": profile, "petName": "小晴（合成数据）",
        "capturedAt": now.isoformat(),
        "runtime": {"aiEnabled": True, "storageReady": True, "busy": False,
                    "pendingMessageCount": 0, "taskStatus": "Completed", "sleeping": False,
                    "daydreamRunning": False,
                    "autonomy": {"enabled": True, "ready": True, "state": "ready",
                                 "isIdle": True, "canDeliberate": True, "paused": False}},
        "emotion": {"enabled": True, "available": True, "active": "joy",
                    "moodValence": .38, "moodArousal": .46, "intensity": .61,
                    "confidence": .9, "updatedAt": now.isoformat()},
        "statistics": {
            "allTime": {key: sum(model[key] for model in models) for key in counters},
            "today": counters, "session": {**counters, "totalTokens": 12000,
                                               "promptTokens": 10000, "completionTokens": 2000},
            "daily": daily, "byModel": models,
            "recentCalls": recent,
            "uptime": {"totalMs": 98765000, "sessionMs": 3600000, "sessionCount": 7,
                       "touchCount": 42, "touchAreaCount": {"head": 12, "body": 30}},
            "legacyImported": False, "persistenceWarning": "",
        },
    }


class OverviewClient(QObject):
    overviewReceived = Signal(dict)
    overviewFailed = Signal(str, str)
    disconnected = Signal()

    def __init__(self):
        super().__init__()
        self.is_connected = True
        self.requests = 0

    def request_runtime_overview(self):
        self.requests += 1


class StatisticsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication(["statistics-tests"])

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="statistics-test-")
        self.root_patch = patch("statistics_data._app_data_dir", return_value=self.directory.name)
        self.root_patch.start()
        setTheme(Theme.LIGHT)
        self.page = StatisticsPage()
        self.page.resize(1000, 850)
        self.page.set_profile(PROFILE, "小晴")
        self.client = OverviewClient()

    def tearDown(self):
        self.page.close()
        self.page.deleteLater()
        self.app.processEvents()
        self.root_patch.stop()
        self.directory.cleanup()

    def online(self):
        self.page.show()
        self.page.set_client(self.client, PROFILE)
        self.client.overviewReceived.emit(sample_snapshot())
        self.app.processEvents()

    def write_saved(self, snapshot=None):
        snapshot = snapshot or sample_snapshot()
        path = profile_directory(PROFILE) / "statistics.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps({"version": 2, "profileId": PROFILE, "petName": "旧名字",
                                    "lastSaved": snapshot["capturedAt"],
                                    "statistics": snapshot["statistics"]}), encoding="utf-8")
        return path

    def test_dashboard_live_ranges_and_emotion(self):
        self.online()
        self.assertIn("50,000", self.page.metric_labels["totalTokens"].text())
        self.assertIn("愉快", self.page.mood_label.text())
        self.assertIn("在线", self.page.status_label.text())
        self.page.range_combo.setCurrentIndex(1)
        self.assertIn("12,000", self.page.metric_labels["totalTokens"].text())
        self.page.range_combo.setCurrentIndex(2)
        self.assertIn("182,000", self.page.metric_labels["totalTokens"].text())
        self.assertEqual(self.page.model_table.rowCount(), 2)
        self.assertEqual(len(self.page.trend_chart.values), 7)
        self.assertEqual(self.page.metric_labels["unknownUsageCount"].text(), "10")
        snapshot = sample_snapshot()
        snapshot["emotion"] = {"enabled": False, "available": False}
        snapshot["runtime"].update(aiEnabled=False, sleeping=True)
        self.client.overviewReceived.emit(snapshot)
        self.assertIn("关闭", self.page.mood_label.text())
        self.assertIn("关闭", self.page.runtime_labels["ai"].text())
        self.assertIn("睡眠", self.page.runtime_labels["activity"].text())
        snapshot["runtime"].update(sleeping=False, busy=False, pendingMessageCount=2)
        self.client.overviewReceived.emit(snapshot)
        self.assertIn("排队", self.page.runtime_labels["activity"].text())
        self.assertNotIn("空闲", self.page.runtime_labels["activity"].text())

    def test_offline_load_no_live_mood_and_recomputes_today(self):
        now = datetime(2026, 10, 21, 12, 0).astimezone()
        saved = sample_snapshot(now=now - timedelta(days=1))
        self.write_saved(saved)
        result, warning = load_saved_statistics(PROFILE, now=now)
        self.assertEqual(warning, "")
        self.assertEqual(result["statistics"]["today"]["totalTokens"], 0)
        self.assertIsNone(result["statistics"]["session"])
        self.assertFalse(result["emotion"]["available"])
        self.assertIsNone(result["runtime"])
        self.page.show()
        self.app.processEvents()
        self.assertFalse(self.page._live)
        self.assertIn("离线", self.page.status_label.text())
        self.assertIn("不可用", self.page.mood_label.text())
        self.page.range_combo.setCurrentIndex(1)
        self.assertEqual(self.page.metric_labels["totalTokens"].text(), "—")

    def test_offline_hidden_then_shown_next_day_recomputes_unchanged_file(self):
        first_day = datetime(2026, 10, 20, 12, 0).astimezone()
        self.write_saved(sample_snapshot(now=first_day))
        with patch("statistics_data.datetime", wraps=datetime) as clock:
            clock.now.return_value = first_day
            self.page.show()
            self.app.processEvents()
            self.assertEqual(self.page.metric_labels["totalTokens"].text(), "50,000")
            self.page.hide()
            clock.now.return_value = first_day + timedelta(days=1)
            self.page.show()
            self.app.processEvents()
            self.assertEqual(self.page.metric_labels["totalTokens"].text(), "0")

    def test_autonomy_states_and_real_task_statuses_are_distinct(self):
        self.online()
        snapshot = sample_snapshot()
        for state, expected in (("failure", "异常"), ("running", "进行中"), ("budget_wait", "预算")):
            snapshot["runtime"]["autonomy"].update(
                state=state, canDeliberate=False, error="STORE_BUSY", reason="storage")
            self.client.overviewReceived.emit(snapshot)
            self.assertIn(expected, self.page.runtime_labels["autonomy"].text())
            self.assertIn("STORE_BUSY", self.page.runtime_labels["autonomy"].toolTip())
        snapshot["runtime"]["taskStatus"] = "WaitingForUser"
        self.client.overviewReceived.emit(snapshot)
        self.assertIn("等待用户", self.page.runtime_details.text())

    def test_bad_file_or_wrong_identity_does_not_display_other_profile(self):
        path = self.write_saved()
        raw = json.loads(path.read_text())
        raw["profileId"] = OTHER_PROFILE
        path.write_text(json.dumps(raw))
        snapshot, warning = load_saved_statistics(PROFILE)
        self.assertIsNone(snapshot)
        self.assertTrue(warning)
        path.write_text("{broken")
        snapshot, warning = load_saved_statistics(PROFILE)
        self.assertIsNone(snapshot)
        self.assertTrue(warning)
        self.assertEqual(path.read_text(), "{broken")
        with self.assertRaises(ValueError):
            profile_directory("../../not-a-profile")

    def test_profile_switch_and_stale_response_unbind_old_client(self):
        self.online()
        self.page.set_profile(OTHER_PROFILE, "另一个角色")
        self.client.overviewReceived.emit(sample_snapshot())
        self.assertFalse(self.page._live)
        self.assertEqual(self.page.metric_labels["totalTokens"].text(), "0")
        new_client = OverviewClient()
        self.page.set_client(new_client, OTHER_PROFILE)
        new_client.overviewReceived.emit(sample_snapshot(OTHER_PROFILE))
        self.assertTrue(self.page._live)
        self.client.overviewFailed.emit("ACTION_UNSUPPORTED", "old result")
        self.client.overviewReceived.emit(sample_snapshot())
        self.assertFalse(self.page._unsupported)
        self.assertEqual(self.page._snapshot["profileId"], OTHER_PROFILE)
        new_client.is_connected = False
        new_client.disconnected.emit()
        self.assertFalse(self.page._live)
        self.assertIn("不可用", self.page.mood_label.text())

    def test_hidden_page_no_poll_and_shown_page_refreshes(self):
        stack = QStackedWidget()
        placeholder = QWidget()
        stack.addWidget(placeholder)
        stack.addWidget(self.page)
        stack.show()
        self.page.set_client(self.client, PROFILE)
        self.assertEqual(self.client.requests, 0)
        self.assertFalse(self.page._poll_timer.isActive())
        stack.setCurrentWidget(self.page)
        self.app.processEvents()
        self.assertEqual(self.client.requests, 1)
        self.assertTrue(self.page._poll_timer.isActive())
        stack.setCurrentWidget(placeholder)
        self.app.processEvents()
        self.page._poll()
        self.assertEqual(self.client.requests, 1)
        self.assertFalse(self.page._poll_timer.isActive())
        stack.setCurrentWidget(self.page)
        self.assertEqual(self.client.requests, 2)
        stack.removeWidget(self.page)
        self.page.setParent(None)
        stack.close()

    def test_unsupported_core_stops_poll_and_does_not_keep_fake_live_state(self):
        self.online()
        self.client.overviewFailed.emit("ACTION_UNSUPPORTED", "unsupported")
        requests = self.client.requests
        self.assertTrue(self.page._unsupported)
        self.assertFalse(self.page._poll_timer.isActive())
        self.assertFalse(self.page._live)
        self.page._poll()
        self.page.hide()
        self.page.show()
        self.assertEqual(self.client.requests, requests)
        self.assertIn("升级", self.page.warning_label.text())

    def test_recent_filters_and_pages(self):
        self.online()
        self.assertEqual(self.page.recent_table.rowCount(), 25)
        self.assertIn("1 / 3", self.page.page_label.text())
        self.page.next_button.click()
        self.assertIn("2 / 3", self.page.page_label.text())
        self.page.role_filter.setCurrentIndex(self.page.role_filter.findData("diary"))
        self.assertEqual(self.page.recent_table.rowCount(), 25)
        self.assertIn("1 / 2", self.page.page_label.text())
        self.page.model_filter.setText("missing-model")
        self.assertEqual(self.page.recent_table.rowCount(), 0)
        self.page.model_filter.clear()
        self.page.result_filter.setCurrentIndex(self.page.result_filter.findData("failure"))
        self.assertGreater(self.page.recent_table.rowCount(), 0)
        self.assertTrue(all(self.page.recent_table.item(row, 3).text() == "失败"
                            for row in range(self.page.recent_table.rowCount())))

    def test_refresh_preserves_range_filter_and_page(self):
        self.online()
        self.page.range_combo.setCurrentIndex(2)
        self.page.role_filter.setCurrentIndex(self.page.role_filter.findData("diary"))
        self.page.model_filter.setText("demo")
        self.page.next_button.click()
        self.client.overviewReceived.emit(sample_snapshot())
        self.assertEqual(self.page.range_combo.currentData(), "allTime")
        self.assertEqual(self.page.role_filter.currentData(), "diary")
        self.assertEqual(self.page.model_filter.text(), "demo")
        self.assertIn("2 / 2", self.page.page_label.text())

    def test_handshake_uses_launched_profile_not_changed_selection(self):
        window = SimpleNamespace(
            _chat_connect_generation=3, launcher_chat_client=self.client,
            _chat_profile_id=PROFILE, state=SimpleNamespace(pet_profile_id=OTHER_PROFILE),
            _append_chat_log=Mock(), chat_page=Mock(), statistics_page=Mock(),
        )
        main.LauncherWindow._on_chat_connected(window, 3, 0, self.client)
        window.statistics_page.set_client.assert_called_once_with(self.client, PROFILE)

    def test_sync_facade_uses_authenticated_overview_action(self):
        client = LauncherChatClient()
        expected = sample_snapshot()
        with patch.object(client, "_authenticated_exchange", return_value=expected) as exchange:
            self.assertEqual(client.get_runtime_overview(), expected)
        exchange.assert_called_once_with("get_runtime_overview", {})

    def test_light_dark_narrow_layout(self):
        self.online()
        for theme in (Theme.LIGHT, Theme.DARK):
            setTheme(theme)
            self.page.refresh_theme()
            self.page.resize(640, 820)
            self.app.processEvents()
            self.assertLessEqual(self.page.view.width(), self.page.viewport().width())
            self.assertTrue(self.page.grab().width() >= 640)
            self.assertGreater(self.page.verticalScrollBar().maximum(), 0)

    def test_core_log_belongs_to_selected_profile(self):
        self.page.set_core_log_path(str(Path(self.directory.name) / "core.log"), PROFILE)
        self.assertTrue(self.page.core_log_button.isEnabled())
        self.page.set_profile(OTHER_PROFILE, "另一个角色")
        self.assertFalse(self.page.core_log_button.isEnabled())


if __name__ == "__main__":
    unittest.main()
