"""Real local-socket regression tests; no model or network API is used."""

import base64
import json
import os
import struct
import sys
import tempfile
import time
import unittest
import uuid
from unittest.mock import patch

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "launcher"))

from PySide6.QtCore import QTimer
from PySide6.QtNetwork import QLocalServer, QLocalSocket
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication

from launcher_chat_client import AsyncLauncherChatClient
import main


TOKEN = base64.urlsafe_b64encode(b"t" * 32).rstrip(b"=").decode("ascii")


class ChatServer:
    def __init__(self):
        self.server = QLocalServer()
        # macOS places relative socket names in a long per-user temp directory.
        self.name = f"chat-{uuid.uuid4().hex[:10]}"
        if not self.server.listen(self.name):
            raise RuntimeError(self.server.errorString())
        self.server.newConnection.connect(self._accept)
        self.sockets = []
        self.timers = []
        self.requests = []
        self.stall = set()
        self.response_error = {}
        self.delayed_poll = False
        self.dribble_poll = False
        self.invalid_poll = False

    def _accept(self):
        while self.server.hasPendingConnections():
            socket = self.server.nextPendingConnection()
            buffer = bytearray()
            self.sockets.append(socket)
            socket.readyRead.connect(lambda s=socket, b=buffer: self._read(s, b))

    def later(self, delay, callback):
        timer = QTimer(self.server)
        timer.setSingleShot(True)
        timer.timeout.connect(callback)
        timer.start(delay)
        self.timers.append(timer)

    def _read(self, socket, buffer):
        buffer.extend(bytes(socket.readAll()))
        while len(buffer) >= 4:
            size = struct.unpack(">I", buffer[:4])[0]
            if len(buffer) < size + 4:
                return
            request = json.loads(bytes(buffer[4:size + 4]))
            del buffer[:size + 4]
            self.requests.append(request)
            action = request["action"]
            if action in self.stall:
                continue
            data = {"sessionToken": TOKEN, "serverVersion": 1} if action == "hello" \
                else {"revision": len(self.requests), "messages": []}
            error = self.response_error.get(action)
            response = {
                "protocolVersion": 1,
                "requestId": request["requestId"],
                "ok": not bool(error),
                "data": data,
                "error": {"code": error or "", "message": "request rejected"},
            }
            if self.invalid_poll and action == "get_chat_state":
                response["requestId"] = "another-request"
            encoded = json.dumps(response).encode("utf-8")
            frame = struct.pack(">I", len(encoded)) + encoded
            if self.dribble_poll and action == "get_chat_state":
                for index, value in enumerate(frame):
                    self.later(index * 20,
                               lambda b=bytes([value]), s=socket: self._send(s, b))
            elif self.delayed_poll and action == "get_chat_state":
                self.delayed_poll = False
                self.later(45, lambda s=socket, f=frame: self._send(s, f))
            else:
                self._send(socket, frame)

    @staticmethod
    def _send(socket, frame):
        if socket.state() == QLocalSocket.ConnectedState:
            socket.write(frame)

    def close(self):
        for timer in self.timers:
            timer.stop()
        for socket in self.sockets:
            socket.abort()
        self.server.close()
        self.server.deleteLater()


class AsyncLauncherChatTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication(["chat-transport-tests"])

    def setUp(self):
        self.server = ChatServer()
        self.client = AsyncLauncherChatClient(timeout_ms=150)
        self.errors = []
        self.states = []
        self.finished = []
        self.disconnects = []
        self.client.operationFailed.connect(lambda *args: self.errors.append(args))
        self.client.stateReceived.connect(self.states.append)
        self.client.operationFinished.connect(lambda *args: self.finished.append(args))
        self.client.disconnected.connect(lambda: self.disconnects.append(True))

    def tearDown(self):
        self.client.close()
        self.client.deleteLater()
        self.server.close()
        self.app.processEvents()

    def wait_for(self, condition, timeout_ms=1500):
        deadline = time.monotonic() + timeout_ms / 1000
        while not condition() and time.monotonic() < deadline:
            QTest.qWait(5)
        self.assertTrue(condition(), "asynchronous condition was not reached")

    def connect_client(self):
        self.client.connect_to_server(self.server.name, TOKEN)
        self.wait_for(lambda: self.client.is_connected)

    def test_connect_and_request_return_before_delayed_reply(self):
        self.server.stall.add("hello")
        started = time.monotonic()
        self.client.connect_to_server(self.server.name, TOKEN)
        self.assertLess(time.monotonic() - started, 0.05)
        self.assertFalse(self.client.is_connected)
        self.wait_for(lambda: bool(self.errors))
        self.assertEqual(self.errors[0][0], "connect")
        self.assertEqual(self.disconnects, [])

    def test_slow_poll_keeps_event_loop_responsive_and_times_out(self):
        self.connect_client()
        self.server.stall.add("get_chat_state")
        ticks = []
        timer = QTimer()
        timer.setInterval(10)
        timer.timeout.connect(lambda: ticks.append(True))
        timer.start()
        self.client.request_state(-1)
        self.wait_for(lambda: bool(self.errors))
        timer.stop()
        self.assertGreaterEqual(len(ticks), 4)
        self.assertEqual(self.errors[0][0], "get_chat_state")
        self.assertFalse(self.client.is_connected)
        self.assertEqual(len(self.disconnects), 1)

    def test_fragmented_response_does_not_extend_total_deadline(self):
        self.connect_client()
        self.server.dribble_poll = True
        started = time.monotonic()
        self.client.request_state(-1)
        self.wait_for(lambda: bool(self.errors))
        self.assertLess(time.monotonic() - started, 0.6)
        self.assertEqual(self.states, [])
        self.assertEqual(len(self.disconnects), 1)

    def test_pending_polls_coalesce_and_user_operations_keep_order(self):
        self.connect_client()
        self.server.delayed_poll = True
        self.client.request_state(-1)
        for revision in range(50):
            self.client.request_state(revision)
        self.client.send_message("hello")
        self.client.stop_response()
        self.wait_for(lambda: len(self.states) == 2 and len(self.finished) == 2)
        self.assertEqual([r["action"] for r in self.server.requests], [
            "hello", "get_chat_state", "send_message", "stop_response",
            "get_chat_state"])
        self.assertEqual(self.server.requests[-1]["payload"]["afterRevision"], 49)
        self.assertEqual(self.errors, [])

    def test_close_and_reconnect_ignore_previous_socket_callbacks(self):
        self.connect_client()
        self.server.stall.add("get_chat_state")
        self.client.request_state(-1)
        self.wait_for(lambda: len(self.server.requests) == 2)
        old_socket, old_generation = self.client._socket, self.client._generation
        self.client.close()
        self.server.stall.clear()
        self.client.connect_to_server(self.server.name, TOKEN)
        self.client._on_disconnected(old_socket, old_generation)
        self.client._on_socket_error(old_socket, old_generation)
        self.wait_for(lambda: self.client.is_connected)
        self.client.request_state(-1)
        self.wait_for(lambda: len(self.states) == 1)
        self.assertEqual(self.disconnects, [])
        self.assertEqual(self.errors, [])

    def test_invalid_response_disconnects_once_and_fails_queued_send(self):
        self.connect_client()
        self.server.invalid_poll = True
        self.client.request_state(-1)
        self.client.send_message("preserve my draft")
        self.wait_for(lambda: bool(self.disconnects))
        self.assertEqual([error[0] for error in self.errors],
                         ["get_chat_state", "send_message"])
        self.assertEqual(len(self.disconnects), 1)
        self.assertFalse(self.client.is_connected)

    def test_non_authentication_error_does_not_drop_connection(self):
        self.connect_client()
        self.server.response_error["send_message"] = "CHAT_BUSY"
        self.client.send_message("hello")
        self.client.request_state(-1)
        self.wait_for(lambda: bool(self.states))
        self.assertEqual(self.errors, [("send_message", "request rejected")])
        self.assertTrue(self.client.is_connected)
        self.assertEqual(self.disconnects, [])

    def test_close_during_handshake_suppresses_timeout_and_error(self):
        self.server.stall.add("hello")
        self.client.connect_to_server(self.server.name, TOKEN)
        self.client.close()
        QTest.qWait(200)
        self.assertEqual(self.errors, [])
        self.assertEqual(self.disconnects, [])

    def test_core_resolution_uses_newest_compatible_build_and_respects_override(self):
        with tempfile.TemporaryDirectory() as directory:
            release = os.path.join(directory, "build", "Release", "Desktop_Pet.exe")
            debug = os.path.join(directory, "build", "Debug", "Desktop_Pet.exe")
            for path, timestamp in ((release, 100), (debug, 200)):
                os.makedirs(os.path.dirname(path))
                with open(path, "wb") as binary:
                    binary.write(b"launcher-chat-bootstrap")
                os.utime(path, (timestamp, timestamp))
            with patch.object(main, "_PROJECT_ROOT", directory), \
                    patch.object(main, "_EXE_SUFFIX", ".exe"), \
                    patch.dict(os.environ):
                os.environ.pop("DESKTOP_PET_EXECUTABLE", None)
                self.assertEqual(main.resolve_cpp_executable(), debug)
                os.environ["DESKTOP_PET_EXECUTABLE"] = release
                self.assertEqual(main.resolve_cpp_executable(), release)

    def test_bootstrap_socket_fits_long_macos_temp_directory(self):
        profile_id = "11111111-1111-4111-8111-111111111111"
        with tempfile.TemporaryDirectory() as directory:
            with patch("main.tempfile.gettempdir", return_value=directory):
                bootstrap = main.LauncherWindow._create_private_bootstrap(
                    None, profile_id, "chat", 1024 * 1024)
            with open(bootstrap["path"], encoding="utf-8") as stream:
                payload = json.load(stream)
            name = bootstrap["socket_name"]
            self.assertRegex(name, r"^dp-c-[a-f0-9]{24}$")
            self.assertEqual(payload["profileId"], profile_id)
            self.assertEqual(payload["capabilityToken"], bootstrap["capability_token"])
            self.assertEqual(payload["socketName"], name)
        if os.name == "posix":
            with tempfile.TemporaryDirectory(dir="/tmp") as directory:
                # sun_path on macOS permits 103 path bytes plus the NUL byte.
                long_temp = os.path.join(directory, "t" * (73 - len(directory) - 1))
                os.mkdir(long_temp)
                socket_path = os.path.join(long_temp, name)
                self.assertEqual(len(os.fsencode(socket_path)), 103)
                server = QLocalServer()
                try:
                    self.assertTrue(server.listen(socket_path), server.errorString())
                finally:
                    server.close()


if __name__ == "__main__":
    unittest.main()
