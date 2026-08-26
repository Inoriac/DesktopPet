import json
import os
import struct
import sys
import unittest
import base64

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LAUNCHER = os.path.join(ROOT, "launcher")
if LAUNCHER not in sys.path:
    sys.path.insert(0, LAUNCHER)

from PySide6.QtCore import QIODeviceBase
from PySide6.QtNetwork import QLocalSocket
from PySide6.QtWidgets import QApplication

from owner_diary_client import OwnerDiaryClient, OwnerDiaryOfflineError
import owner_diary_client
from pages.private_diary_page import PrivateDiaryPage


class FakeSignal:
    def __init__(self):
        self.callbacks = []

    def connect(self, callback):
        self.callbacks.append(callback)

    def emit(self):
        for callback in list(self.callbacks):
            callback()


class FakeLocalSocket:
    def __init__(self, available=True):
        self.available = available
        self.connected = False
        self.incoming = bytearray()
        self.actions = []
        self.disconnected = FakeSignal()
        self.error_by_action = {}
        self.disconnect_on_action = set()
        self.session_token = base64.urlsafe_b64encode(
            b"s" * 32).rstrip(b"=").decode("ascii")

    def connectToServer(self, _name, _mode=QIODeviceBase.ReadWrite):
        self.connected = self.available

    def waitForConnected(self, _timeout):
        return self.connected

    def state(self):
        state_enum = getattr(QLocalSocket, "LocalSocketState", QLocalSocket)
        return (state_enum.ConnectedState if self.connected
                else state_enum.UnconnectedState)

    def write(self, frame):
        raw = bytes(frame)
        size = struct.unpack(">I", raw[:4])[0]
        request = json.loads(raw[4:4 + size].decode("utf-8"))
        action = request["action"]
        self.actions.append(action)
        if action in self.disconnect_on_action:
            self.force_disconnect()
            return -1
        error_code = self.error_by_action.get(action)
        if action == "hello":
            data = {"sessionToken": self.session_token, "serverVersion": 1}
        elif action == "list_diary_entries":
            data = {
                "entries": [{
                    "entryId": "entry-1",
                    "localDate": "2026-08-25",
                    "index": {"theme": "companionship"},
                    "createdAt": "2026-08-25T23:30:00.000Z",
                }],
                "nextCursor": "",
            }
        elif action == "get_diary_entry":
            data = {
                "entryId": "entry-1",
                "localDate": "2026-08-25",
                "body": "private diary body",
                "index": {"theme": "companionship"},
                "createdAt": "2026-08-25T23:30:00.000Z",
            }
        else:
            data = {}
        response = json.dumps({
            "protocolVersion": 1,
            "requestId": request["requestId"],
            "ok": error_code is None,
            "data": data if error_code is None else {},
            "error": {
                "code": error_code or "",
                "message": "request failed" if error_code else "",
                "retryable": False,
            },
        }, separators=(",", ":")).encode("utf-8")
        self.incoming.extend(struct.pack(">I", len(response)) + response)
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

    def force_disconnect(self):
        self.disconnectFromServer()


class OwnerDiaryClientTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication(["owner-diary-tests"])

    @staticmethod
    def token():
        return base64.urlsafe_b64encode(b"a" * 32).rstrip(b"=").decode("ascii")

    def test_connect_then_list_and_get_whenServerAvailable_shouldRenderMetadataThenSingleBody(self):
        socket = FakeLocalSocket()
        client = OwnerDiaryClient(socket_factory=lambda: socket)
        client.connect("owner-socket", self.token())
        page = PrivateDiaryPage()
        page.set_client(client)
        page.activate()

        self.assertEqual(page.entry_list.count(), 1)
        self.assertNotIn("private diary body", page.entry_list.item(0).text())
        page.entry_list.setCurrentRow(0)
        page.load_selected_entry()
        self.assertIn("private diary body", page.body_view.toPlainText())
        self.assertEqual(socket.actions,
                         ["hello", "list_diary_entries", "get_diary_entry"])
        socket.error_by_action["get_diary_entry"] = "DIARY_ENTRY_NOT_FOUND"
        with self.assertRaises(owner_diary_client.OwnerDiaryProtocolError):
            client.get_entry("missing-entry")
        self.assertTrue(client.is_connected)
        self.assertIsNotNone(client.session_token)

    def test_pageDeactivate_whenBodyLoaded_shouldClearContentAndKeepLauncherSession(self):
        socket = FakeLocalSocket()
        client = OwnerDiaryClient(socket_factory=lambda: socket)
        client.connect("owner-socket", self.token())
        page = PrivateDiaryPage()
        page.set_client(client)
        page.activate()
        page.entry_list.setCurrentRow(0)
        page.load_selected_entry()
        page.deactivate()

        self.assertEqual(page.body_view.toPlainText(), "")
        self.assertTrue(client.is_connected)
        self.assertIsNotNone(client.session_token)

        page.activate()
        page.entry_list.setCurrentRow(0)
        page.load_selected_entry()
        self.assertNotEqual(page.body_view.toPlainText(), "")
        socket.disconnect_on_action.add("get_diary_entry")
        page.load_selected_entry()
        self.assertEqual(page.body_view.toPlainText(), "")
        self.assertIsNone(client.session_token)
        self.assertIn("离线", page.status_label.text())

    def test_close_whenLauncherExits_shouldClearTokenAndDecryptedContent(self):
        socket = FakeLocalSocket()
        client = OwnerDiaryClient(socket_factory=lambda: socket)
        client.connect("owner-socket", self.token())
        page = PrivateDiaryPage()
        page.set_client(client)
        page.activate()
        page.entry_list.setCurrentRow(0)
        page.load_selected_entry()
        page.deactivate()
        client.close()

        self.assertIsNone(client.session_token)
        self.assertIsNone(client.capability_token)
        self.assertEqual(page.body_view.toPlainText(), "")
        self.assertFalse(socket.connected)

    def test_request_whenServerOffline_shouldShowOfflineWithoutOpeningSQLite(self):
        socket = FakeLocalSocket(available=False)
        client = OwnerDiaryClient(socket_factory=lambda: socket)
        with self.assertRaises(OwnerDiaryOfflineError):
            client.connect("missing-socket", self.token())
        page = PrivateDiaryPage()
        page.set_client(client)
        page.activate()

        self.assertIn("离线", page.status_label.text())
        self.assertEqual(socket.actions, [])
        self.assertFalse(hasattr(owner_diary_client, "sqlite3"))

    def test_connect_whenLauncherWasRestartedWithOldCoreRunning_shouldNotReuseConsumedTokenOrGuessSocket(self):
        old_socket = FakeLocalSocket()
        old_client = OwnerDiaryClient(socket_factory=lambda: old_socket)
        old_client.connect("old-random-socket", self.token())
        old_client.close()

        new_socket = FakeLocalSocket(available=False)
        restarted_client = OwnerDiaryClient(socket_factory=lambda: new_socket)
        with self.assertRaises(OwnerDiaryOfflineError):
            restarted_client.list_entries("", "", None, 20)
        self.assertIsNone(restarted_client.socket_name)
        self.assertIsNone(restarted_client.capability_token)
        self.assertEqual(new_socket.actions, [])


if __name__ == "__main__":
    unittest.main()
