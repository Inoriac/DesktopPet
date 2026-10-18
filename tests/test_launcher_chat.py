import base64
import json
import os
import struct
import sys
import unittest
from unittest.mock import patch

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LAUNCHER = os.path.join(ROOT, "launcher")
if LAUNCHER not in sys.path:
    sys.path.insert(0, LAUNCHER)

from PySide6.QtCore import QIODeviceBase, QObject, QPoint, QPointF, Signal, Qt
from PySide6.QtTest import QTest
from PySide6.QtGui import QTextCursor, QWheelEvent
from PySide6.QtNetwork import QLocalSocket
from PySide6.QtWidgets import QApplication, QAbstractSlider

from launcher_chat_client import LauncherChatClient
from pages.chat_page import ChatPage, InfoBar


class FakeSignal:
    def __init__(self):
        self.callbacks = []

    def connect(self, callback):
        self.callbacks.append(callback)

    def emit(self):
        for callback in list(self.callbacks):
            callback()


class FakeLocalSocket:
    def __init__(self):
        self.connected = False
        self.incoming = bytearray()
        self.actions = []
        self.disconnected = FakeSignal()
        self.session_token = base64.urlsafe_b64encode(
            b"s" * 32).rstrip(b"=").decode("ascii")

    def connectToServer(self, _name, _mode=QIODeviceBase.ReadWrite):
        self.connected = True

    def waitForConnected(self, _timeout):
        return self.connected

    def state(self):
        state_enum = getattr(QLocalSocket, "LocalSocketState", QLocalSocket)
        return state_enum.ConnectedState if self.connected \
            else state_enum.UnconnectedState

    def write(self, frame):
        raw = bytes(frame)
        size = struct.unpack(">I", raw[:4])[0]
        request = json.loads(raw[4:4 + size].decode("utf-8"))
        action = request["action"]
        self.actions.append(action)
        if action == "hello":
            data = {"sessionToken": self.session_token, "serverVersion": 1}
        elif action == "get_chat_state":
            data = {
                "revision": 2,
                "openRequestId": 0,
                "unchanged": False,
                "petName": "Milltina",
                "aiEnabled": True,
                "busy": False,
                "messages": [],
                "statistics": {"callCount": 3, "successCount": 2,
                               "failureCount": 1, "totalTokens": 99},
            }
        else:
            data = {"messageId": "message-1"} if action != "stop_response" else {}
        encoded = json.dumps({
            "protocolVersion": 1,
            "requestId": request["requestId"],
            "ok": True,
            "data": data,
        }, separators=(",", ":")).encode("utf-8")
        self.incoming.extend(struct.pack(">I", len(encoded)) + encoded)
        return len(raw)

    def waitForBytesWritten(self, _timeout):
        return self.connected

    def bytesAvailable(self):
        return len(self.incoming)

    def waitForReadyRead(self, _timeout):
        return bool(self.incoming)

    def read(self, size):
        chunk = bytes(self.incoming[:size])
        del self.incoming[:size]
        return chunk

    def disconnectFromServer(self):
        was_connected = self.connected
        self.connected = False
        if was_connected:
            self.disconnected.emit()

    def waitForDisconnected(self, _timeout):
        return True

    def abort(self):
        self.connected = False
        self.incoming.clear()


class StubChatClient(QObject):
    disconnected = Signal()
    stateReceived = Signal(dict)
    operationFinished = Signal(str, dict)
    operationFailed = Signal(str, str)
    historyFailed = Signal(str, str, str)

    def __init__(self):
        super().__init__()
        self.is_connected = True
        self.revision = 1
        self.open_request_id = 0
        self.busy = False
        self.sent = []
        self.stop_calls = 0
        self.task_calls = []
        self.history_calls = []

    def request_state(self, after_revision=-1):
        self.stateReceived.emit(self.get_state(after_revision))

    def get_state(self, after_revision=-1):
        if after_revision == self.revision:
            return {"revision": self.revision,
                    "openRequestId": self.open_request_id,
                    "unchanged": True}
        return {
            "revision": self.revision,
            "openRequestId": self.open_request_id,
            "unchanged": False,
            "petName": "Milltina",
            "aiEnabled": True,
            "busy": self.busy,
            "messages": [{"id": "u1", "role": "user", "content": "你好",
                          "status": "complete"}],
            "statistics": {"callCount": 3, "successCount": 2,
                           "failureCount": 1, "totalTokens": 99},
        }

    def request_history(self, before_message_id="", around_message_id="", limit=40, request_token=""):
        self.history_calls.append({"beforeMessageId": before_message_id,
                                   "aroundMessageId": around_message_id, "limit": limit,
                                   "requestToken": request_token})

    def send_message(self, text):
        self.sent.append(text)
        self.operationFinished.emit("send_message", {"messageId": "u2"})

    def stop_response(self):
        self.stop_calls += 1
        self.operationFinished.emit("stop_response", {})

    def retry_message(self, _message_id):
        return {}

    def resume_task(self, task_id, feedback="", extend_budget=False):
        self.task_calls.append(("resume_task", task_id, feedback, extend_budget))

    def pause_task(self, task_id):
        self.task_calls.append(("pause_task", task_id))

    def stop_task(self, task_id):
        self.task_calls.append(("stop_task", task_id))

    def resolve_task_call(self, task_id, call_id, succeeded):
        self.task_calls.append(("resolve_task_call", task_id, call_id, succeeded))


class LauncherChatTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication(["launcher-chat-tests"])

    def page(self):
        page = ChatPage()
        page.resize(840, 640)
        page.show()
        self.addCleanup(page.deleteLater)
        self.addCleanup(page.close)
        self.addCleanup(lambda: page.set_client(None))
        return page

    def settle(self):
        for _ in range(5):
            self.app.processEvents()
        QTest.qWait(20)

    @staticmethod
    def state(messages, busy=False, revision=1, has_more=False, profile="qa", diagnostics=None):
        return {"revision": revision, "profileId": profile, "petName": "QA", "busy": busy,
                "aiEnabled": True, "messages": messages, "hasMoreBefore": has_more,
                "hasMoreAfter": False, "diagnosticMessages": diagnostics or []}

    def history_page(self, page, client, messages, has_more=False):
        request = client.history_calls[-1]
        client.operationFinished.emit("get_chat_history", {
            "requestToken": request["requestToken"], "messages": messages,
            "hasMoreBefore": has_more, "hasMoreAfter": True,
            "oldestMessageId": messages[0]["id"] if messages else "",
            "newestMessageId": messages[-1]["id"] if messages else ""})
        self.settle()

    def history_setup(self):
        page = self.page()
        client = StubChatClient()
        page.set_client(client)
        page._poll_timer.stop()
        page._apply_state(self.state(self.messages(180)[-40:], revision=2, has_more=True))
        self.settle()
        return page, client

    def wheel_up(self, page, delta=120):
        viewport = page.message_area.viewport()
        event = QWheelEvent(QPointF(50, 50), QPointF(viewport.mapToGlobal(QPoint(50, 50))),
                            QPoint(), QPoint(0, delta), Qt.NoButton, Qt.NoModifier,
                            Qt.NoScrollPhase, False)
        QApplication.sendEvent(viewport, event)
        self.settle()

    @staticmethod
    def messages(count):
        return [{"id": str(i), "role": "user" if i % 2 else "assistant",
                 "content": f"消息 {i}：" + "验收内容 " * 18,
                 "status": "complete"} for i in range(count)]

    @staticmethod
    def token():
        return base64.urlsafe_b64encode(
            b"a" * 32).rstrip(b"=").decode("ascii")

    def test_client_whenConnected_shouldUseChatActions(self):
        socket = FakeLocalSocket()
        client = LauncherChatClient(socket_factory=lambda: socket)
        client.connect_to_server("chat-socket", self.token())

        state = client.get_state(-1)
        client.send_message("hello")
        client.stop_response()

        self.assertEqual(state["statistics"]["totalTokens"], 99)
        self.assertEqual(socket.actions,
                         ["hello", "get_chat_state", "send_message",
                          "stop_response"])

    def test_task_resume_keeps_counts_and_extends_only_on_explicit_button(self):
        client = StubChatClient()
        page = self.page()
        page.set_client(client)
        page._poll_timer.stop()
        state = self.state([], revision=2)
        state["task"] = {"id": "task-1", "goal": "比较资料", "status": "Paused",
                         "reason": "已保存进度", "tool_calls": 12,
                         "budget": {"max_tool_calls": 12, "max_replans": 2, "max_active_ms": 300000}}
        page._apply_state(state)
        self.assertTrue(page.task_continue.isVisible())
        self.assertTrue(page.task_extend.isVisible())
        page.input_edit.setPlainText("请增加来源")
        page.task_continue.click()
        self.assertEqual(client.task_calls, [("resume_task", "task-1", "请增加来源", False)])
        self.assertFalse(page.task_continue.isEnabled())
        self.assertEqual(page.input_edit.toPlainText(), "请增加来源")
        client.revision = 2
        page._operation_finished("resume_task", {})
        page.task_extend.click()
        self.assertEqual(client.task_calls[-1], ("resume_task", "task-1", "", True))

    def test_unknown_action_requires_explicit_outcome_and_offline_disables_controls(self):
        client = StubChatClient()
        page = self.page()
        page.set_client(client)
        page._poll_timer.stop()
        state = self.state([], revision=2)
        state["task"] = {"id": "task-1", "status": "WaitingForUser", "goal": "提醒",
                         "steps": [{"id": "call-1", "status": "unknown", "read_only": False}]}
        page._apply_state(state)
        self.assertFalse(page.task_continue.isVisible())
        self.assertTrue(page.task_confirm_done.isVisible())
        page.task_confirm_done.click()
        self.assertEqual(client.task_calls, [("resolve_task_call", "task-1", "call-1", True)])
        client.is_connected = False
        client.disconnected.emit()
        self.assertFalse(page.task_confirm_done.isEnabled())
        self.assertFalse(page.task_stop.isEnabled())

    def test_page_whenStateChanges_shouldRenderStatsSendStopAndOpen(self):
        client = StubChatClient()
        page = self.page()
        opened = []
        page.openRequested.connect(lambda: opened.append(True))
        page.set_client(client)

        self.assertEqual(page.failure_count_label.text(), "失败 1")
        self.assertEqual(page.token_count_label.text(), "Token 99")
        page.input_edit.setPlainText("  在吗  ")
        page._submit()
        self.assertEqual(client.sent, ["在吗"])

        client.busy = True
        client.open_request_id = 1
        client.revision += 1
        page.poll_once()
        page._stop()
        self.assertEqual(client.stop_calls, 1)
        self.assertEqual(opened, [True])

    def test_page_whenTransportDisconnects_shouldRequestReconnect(self):
        client = StubChatClient()
        page = self.page()
        failures = []
        page.connectionLost.connect(failures.append)
        page.set_client(client)

        client.is_connected = False
        client.disconnected.emit()

        self.assertEqual(failures, ["聊天连接已断开"])
        self.assertEqual(page.status_label.text(), "离线")
        self.assertFalse(page.action_button.isEnabled())

    def test_recent_page_starts_at_bottom_and_layout_never_requests_history(self):
        page, client = self.history_setup()
        bar = page.message_area.verticalScrollBar()
        self.assertEqual(len(page._rows), 40)
        self.assertEqual(bar.value(), bar.maximum())
        self.assertEqual(client.history_calls, [])
        page.resize(700, 700)
        self.settle()
        bar.setValue(0)  # programmatic restoration is not user input
        self.settle()
        self.assertEqual(client.history_calls, [])
        self.assertTrue(page.history_button.isVisible())

    def test_user_up_scroll_requests_one_page_and_keeps_visible_anchor(self):
        page, client = self.history_setup()
        bar = page.message_area.verticalScrollBar()
        bar.setValue(0)
        row = page._rows["140"]
        old_y = row.y() - bar.value()
        self.wheel_up(page)
        self.wheel_up(page)
        self.assertEqual(len(client.history_calls), 1)
        self.assertEqual(client.history_calls[0]["beforeMessageId"], "140")
        self.assertEqual(client.history_calls[0]["limit"], 40)
        self.assertEqual(len(page._rows), 40)
        self.history_page(page, client, self.messages(140)[-40:], has_more=True)
        self.assertEqual(len(page._rows), 80)
        self.assertLessEqual(abs(row.y() - bar.value() - old_y), 2)
        self.assertEqual(len(client.history_calls), 1)
        bar.setValue(0)
        self.wheel_up(page)
        self.history_page(page, client, self.messages(100)[-40:], has_more=True)
        self.assertEqual(len(page._rows), 120)
        self.assertEqual(client.history_calls[-1]["beforeMessageId"], "100")

    def test_keyboard_and_scrollbar_actions_can_request_history(self):
        for action in ("keyboard", "scrollbar"):
            with self.subTest(action=action):
                page, client = self.history_setup()
                bar = page.message_area.verticalScrollBar()
                bar.setValue(0)
                if action == "keyboard":
                    QTest.keyClick(page.message_area, Qt.Key_PageUp)
                else:
                    bar.triggerAction(QAbstractSlider.SliderToMinimum)
                self.settle()
                self.assertEqual(len(client.history_calls), 1)

    def test_paged_history_survives_stream_updates_without_stealing_scroll_or_selection(self):
        page, client = self.history_setup()
        page.message_area.verticalScrollBar().setValue(0)
        page._load_older()
        self.history_page(page, client, self.messages(140)[-40:], has_more=True)
        bar = page.message_area.verticalScrollBar()
        row = page._rows["140"]
        row.text_label.setSelection(0, 3)
        position = bar.value()
        messages = self.messages(181)[-40:]
        messages[-1] = dict(messages[-1], content="新消息 " * 50, status="streaming")
        page._apply_state(self.state(messages, busy=True, revision=3, has_more=True))
        self.settle()
        self.assertEqual(len(page._rows), 81)
        self.assertIs(page._rows["140"], row)
        self.assertEqual(row.text_label.selectedText(), "消息 ")
        self.assertEqual(bar.value(), position)
        self.assertTrue(page.jump_button.isVisible())
        page.scroll_to_bottom()
        self.settle()
        self.assertEqual(set(page._rows), {str(i) for i in range(141, 181)})
        self.assertTrue(page._follow_bottom)
        self.assertEqual(bar.value(), bar.maximum())

    def test_history_failure_can_retry_and_old_profile_results_are_discarded(self):
        page, client = self.history_setup()
        page._load_older()
        token = client.history_calls[-1]["requestToken"]
        client.historyFailed.emit("get_chat_history", token, "Connection timeout")
        self.assertTrue(page.history_button.isEnabled())
        self.assertIn("重试", page.history_button.text())
        self.assertEqual(len(page._rows), 40)
        page.history_button.click()
        stale = client.history_calls[-1]["requestToken"]
        page._apply_state(self.state([{"id": "new-profile", "role": "user", "content": "新角色", "status": "complete"}],
                                    revision=3, profile="other"))
        client.operationFinished.emit("get_chat_history", {"requestToken": stale,
                                      "messages": self.messages(140)[-40:], "hasMoreBefore": True})
        client.historyFailed.emit("get_chat_history", stale, "late error")
        self.settle()
        self.assertEqual(set(page._rows), {"new-profile"})
        self.assertNotIn("重试", page.history_button.text())

    def test_downward_user_scroll_cancels_pending_anchor_restore_without_loading(self):
        for action in ("end", "wheel", "scrollbar"):
            with self.subTest(action=action):
                page, client = self.history_setup()
                bar = page.message_area.verticalScrollBar()
                bar.setValue(bar.maximum() // 3)
                position = bar.value()
                messages = self.messages(180)[-40:]
                messages[-1] = dict(messages[-1], content="更新的长回复 " * 60, status="streaming")
                page._apply_state(self.state(messages, revision=3, has_more=True))
                if action == "end":
                    QTest.keyClick(page.message_area, Qt.Key_End)
                elif action == "wheel":
                    self.wheel_up(page, -120)
                else:
                    bar.triggerAction(QAbstractSlider.SliderToMaximum)
                self.settle()
                self.assertGreater(bar.value(), position)
                if action != "wheel":
                    self.assertEqual(bar.value(), bar.maximum())
                self.assertEqual(client.history_calls, [])

    def test_pending_failure_moves_to_diagnostics_and_retains_retry_notice(self):
        page = self.page()
        pending = {"id": "answer", "role": "assistant", "status": "pending", "content": ""}
        page._apply_state(self.state([pending], busy=True))
        self.settle()
        self.assertIn("answer", page._rows)
        failed = dict(pending, status="failed", errorMessage="network error")
        with patch("pages.chat_page.InfoBar.error", wraps=InfoBar.error) as notice:
            page._apply_state(self.state([], revision=2, diagnostics=[failed]))
            page._apply_state(self.state([], revision=3, diagnostics=[failed]))
            self.settle()
            self.assertEqual(notice.call_count, 1)
        self.assertEqual(page._rows, {})
        self.assertIn("network error", page.diagnostics_text.toPlainText())

    def test_recent_snapshot_gap_does_not_fake_adjacency_while_reading(self):
        page, _client = self.history_setup()
        bar = page.message_area.verticalScrollBar()
        bar.setValue(0)
        page._apply_state(self.state(self.messages(260)[-40:], revision=3, has_more=True))
        self.settle()
        self.assertEqual(set(page._rows), {str(i) for i in range(140, 180)})
        self.assertTrue(page.jump_button.isVisible())
        page.scroll_to_bottom()
        self.settle()
        self.assertEqual(set(page._rows), {str(i) for i in range(220, 260)})

    def test_disconnect_keeps_loaded_messages_and_draft_during_resize(self):
        page, client = self.history_setup()
        page.input_edit.setPlainText("还没发送的消息")
        page._load_older()
        token = client.history_calls[-1]["requestToken"]
        page.set_client(None)
        page.resize(700, 700)
        client.operationFinished.emit("get_chat_history", {"requestToken": token,
                                      "messages": self.messages(140)[-40:], "hasMoreBefore": True})
        self.settle()
        self.assertEqual(len(page._rows), 40)
        self.assertEqual(page.input_edit.toPlainText(), "还没发送的消息")
        self.assertFalse(page.history_button.isEnabled())
        self.assertFalse(page.action_button.isEnabled())

    def test_qq_layout_shows_avatars_time_and_fits_narrow_task_panel(self):
        page, _client = self.history_setup()
        state = self.state([dict(self.messages(1)[0], timestamp="2026-10-18T10:30:00+08:00")], revision=3)
        state["task"] = {"id": "task-1", "status": "WaitingForUser", "goal": "核对播放结果",
                         "steps": [{"id": "call-1", "status": "unknown", "read_only": False}]}
        page._apply_state(state)
        page.resize(460, 700)  # 700px launcher minus navigation
        self.settle()
        row = page._rows["0"]
        self.assertTrue(row.avatar.isVisible())
        self.assertTrue(row.time_label.text())
        self.assertLessEqual(page.minimumSizeHint().width(), 460)
        self.assertEqual(page.message_area.horizontalScrollBar().maximum(), 0)

    def test_enter_busy_sends_each_message_without_stopping_stream(self):
        client = StubChatClient()
        client.busy = True
        page = self.page()
        page.set_client(client)
        for text in ("我明天", "要去上海出差", "待两天"):
            page.input_edit.setPlainText(text)
            QTest.keyClick(page.input_edit, Qt.Key_Return)
            self.assertEqual(page.input_edit.toPlainText(), "")
        self.assertEqual(client.stop_calls, 0)
        self.assertEqual(client.sent, ["我明天", "要去上海出差", "待两天"])
        self.assertTrue(page.stop_button.isEnabled())

    def test_failed_send_preserves_draft_and_uses_notice_not_bubble(self):
        client = StubChatClient()
        client.send_message = lambda _text: None
        page = self.page()
        page.set_client(client)
        page.input_edit.setPlainText("请保留这个草稿")
        page._submit()
        with patch("pages.chat_page.InfoBar.error", wraps=InfoBar.error) as notice:
            client.operationFailed.emit("send_message", "Connection closed")
        self.assertEqual(page.input_edit.toPlainText(), "请保留这个草稿")
        self.assertTrue(page.action_button.isEnabled())
        self.assertEqual(notice.call_count, 1)
        self.assertNotIn("Connection closed", str([r.message for r in page._rows.values()]))
        self.assertIn("Connection closed", page.diagnostics_text.toPlainText())

    def test_system_and_failure_details_are_outside_conversation(self):
        page = self.page()
        messages = [{"id": "sys", "role": "system", "content": "内部诊断", "status": "complete"},
                    {"id": "old", "role": "assistant", "content": "", "status": "failed",
                     "errorMessage": "old network error"},
                    {"id": "u", "role": "user", "content": "<b>不是 HTML</b>", "status": "complete"}]
        with patch("pages.chat_page.InfoBar.error", wraps=InfoBar.error) as notice:
            page._apply_state(self.state(messages[-1:], diagnostics=messages[:-1]))
            self.assertEqual(notice.call_count, 0)
            messages.append({"id": "new", "role": "assistant", "content": "",
                             "status": "failed", "errorMessage": "new network error"})
            page._apply_state(self.state([messages[2]], revision=2, diagnostics=messages[:2] + messages[3:]))
            page._apply_state(self.state([messages[2]], revision=3, diagnostics=messages[:2] + messages[3:]))
            self.assertEqual(notice.call_count, 1)
        self.assertEqual(set(page._rows), {"u"})
        self.assertEqual(page._rows["u"].text_label.textFormat(), Qt.PlainText)
        self.assertTrue(page.diagnostics_panel.isHidden())
        self.assertIn("内部诊断", page.diagnostics_text.toPlainText())
        self.assertIn("new network error", page.diagnostics_text.toPlainText())

    def test_explicit_stop_and_shift_enter_do_not_send(self):
        page = self.page()
        client = StubChatClient()
        client.busy = True
        page.set_client(client)
        page.input_edit.setPlainText("第一行")
        page.input_edit.moveCursor(QTextCursor.End)
        QTest.keyClick(page.input_edit, Qt.Key_Return, Qt.ShiftModifier)
        self.assertIn("\n", page.input_edit.toPlainText())
        page._stop()
        self.assertEqual(client.sent, [])
        self.assertEqual(client.stop_calls, 1)
        self.assertTrue(page.input_edit.toPlainText())


if __name__ == "__main__":
    unittest.main()
