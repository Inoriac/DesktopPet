import base64
import json
import os
import struct
import sys
import unittest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LAUNCHER = os.path.join(ROOT, "launcher")
if LAUNCHER not in sys.path:
    sys.path.insert(0, LAUNCHER)

from PySide6.QtCore import QIODeviceBase, QObject, Signal
from PySide6.QtNetwork import QLocalSocket
from PySide6.QtWidgets import QApplication

from launcher_chat_client import LauncherChatClient
from pages.chat_page import ChatPage


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

    def __init__(self):
        super().__init__()
        self.is_connected = True
        self.revision = 1
        self.open_request_id = 0
        self.busy = False
        self.sent = []
        self.stop_calls = 0

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
        return {"messageId": "u2"}

    def stop_response(self):
        self.stop_calls += 1
        return {}

    def retry_message(self, _message_id):
        return {}


class LauncherChatTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication(["launcher-chat-tests"])

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
        page = ChatPage()
        opened = []
        page.openRequested.connect(lambda: opened.append(True))
        page.set_client(client)

        self.assertEqual(page.failure_count_label.text(), "失败 1")
        self.assertEqual(page.token_count_label.text(), "Token 99")
        page.input_edit.setPlainText("  在吗  ")
        page._submit_or_stop()
        self.assertEqual(client.sent, ["在吗"])

        client.busy = True
        client.open_request_id = 1
        client.revision += 1
        page.poll_once()
        page._submit_or_stop()
        self.assertEqual(client.stop_calls, 1)
        self.assertEqual(opened, [True])

    def test_page_whenTransportDisconnects_shouldRequestReconnect(self):
        client = StubChatClient()
        page = ChatPage()
        failures = []
        page.connectionLost.connect(failures.append)
        page.set_client(client)

        client.is_connected = False
        client.disconnected.emit()

        self.assertEqual(failures, ["聊天连接已断开"])
        self.assertEqual(page.status_label.text(), "离线")
        self.assertFalse(page.action_button.isEnabled())


if __name__ == "__main__":
    unittest.main()
