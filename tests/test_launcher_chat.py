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

from PySide6.QtCore import QIODeviceBase, QObject, Signal, Qt
from PySide6.QtTest import QTest
from PySide6.QtGui import QTextCursor
from PySide6.QtNetwork import QLocalSocket
from PySide6.QtWidgets import QApplication

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

    def __init__(self):
        super().__init__()
        self.is_connected = True
        self.revision = 1
        self.open_request_id = 0
        self.busy = False
        self.sent = []
        self.stop_calls = 0

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

    def send_message(self, text):
        self.sent.append(text)
        self.operationFinished.emit("send_message", {"messageId": "u2"})

    def stop_response(self):
        self.stop_calls += 1
        self.operationFinished.emit("stop_response", {})

    def retry_message(self, _message_id):
        return {}


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
    def state(messages, busy=False, revision=1):
        return {"revision": revision, "petName": "QA", "busy": busy,
                "aiEnabled": True, "messages": messages}

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

    def test_history_starts_at_bottom_and_stream_updates_preserve_rows(self):
        page = self.page()
        messages = self.messages(120)
        page._apply_state(self.state(messages))
        self.settle()
        bar = page.message_area.verticalScrollBar()
        self.assertEqual(len(page._rows), page.PAGE_SIZE)
        self.assertGreater(bar.maximum(), 100)
        self.assertEqual(bar.value(), bar.maximum())
        last_row = page._rows["119"]
        last_y = last_row.mapTo(page.message_area.viewport(), last_row.rect().bottomLeft()).y()
        self.assertGreater(last_y, 0)
        self.assertLessEqual(last_y, page.message_area.viewport().height())
        self.assertTrue(page.history_button.isVisible())
        previous_row = page._rows["100"]
        bar.setValue(bar.maximum() // 3)
        old_position = bar.value()
        messages[-1] = dict(messages[-1], content="追加的流式回复 " * 45,
                            status="streaming")
        page._apply_state(self.state(messages, busy=True, revision=2))
        self.settle()
        self.assertIs(page._rows["100"], previous_row)
        self.assertEqual(bar.value(), old_position)
        self.assertTrue(page.jump_button.isVisible())
        QTest.mouseClick(page.jump_button, Qt.LeftButton)
        self.settle()
        self.assertEqual(bar.value(), bar.maximum())
        self.assertFalse(page.jump_button.isVisible())

    def test_new_message_and_pending_restore_do_not_override_reader_intent(self):
        page = self.page()
        messages = self.messages(120)
        page._apply_state(self.state(messages))
        self.settle()
        bar = page.message_area.verticalScrollBar()
        bar.setValue(0)
        first_row = page._rows["80"]
        old_y = first_row.y() - bar.value()
        messages.append({"id": "120", "role": "assistant", "content": "新消息",
                         "status": "complete"})
        page._apply_state(self.state(messages[-120:], revision=2))
        self.settle()
        self.assertIs(page._rows["80"], first_row)
        self.assertEqual(first_row.y() - bar.value(), old_y)
        messages[-1] = dict(messages[-1], content="变长的新消息 " * 200)
        page._apply_state(self.state(messages[-120:], revision=3))
        page.scroll_to_bottom()
        self.settle()
        self.assertTrue(page._follow_bottom)
        self.assertEqual(bar.value(), bar.maximum())

    def test_loading_older_preserves_view_and_newest_can_be_reached(self):
        page = self.page()
        page._apply_state(self.state(self.messages(120)))
        self.settle()
        bar = page.message_area.verticalScrollBar()
        bar.setValue(0)
        row = page._rows["80"]
        old_y = row.mapTo(page.message_area.viewport(), row.rect().topLeft()).y()
        page._load_older()
        self.settle()
        self.assertEqual(len(page._rows), 80)
        new_y = row.mapTo(page.message_area.viewport(), row.rect().topLeft()).y()
        self.assertLessEqual(abs(new_y - old_y), 2)
        page.scroll_to_bottom()
        self.settle()
        self.assertEqual(bar.value(), bar.maximum())

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
            page._apply_state(self.state(messages))
            self.assertEqual(notice.call_count, 0)
            messages.append({"id": "new", "role": "assistant", "content": "",
                             "status": "failed", "errorMessage": "new network error"})
            page._apply_state(self.state(messages, revision=2))
            page._apply_state(self.state(messages, revision=3))
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
