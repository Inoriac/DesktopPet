"""Authenticated Launcher client for the C++ chat bridge."""

from __future__ import annotations

from collections import deque
import json
import struct
import uuid
from typing import Callable

from PySide6.QtCore import QIODeviceBase, QObject, QTimer, Signal
from PySide6.QtNetwork import QLocalSocket

from owner_diary_client import (
    OwnerDiaryClient,
    OwnerDiaryError,
    OwnerDiaryOfflineError,
    OwnerDiaryProtocolError,
    _base64url_token,
    _decode_token,
)


LauncherChatError = OwnerDiaryError
LauncherChatOfflineError = OwnerDiaryOfflineError
LauncherChatProtocolError = OwnerDiaryProtocolError


class LauncherChatClient(OwnerDiaryClient):
    """Small typed facade over the shared authenticated local transport."""

    def __init__(
        self,
        socket_factory: Callable[[], QLocalSocket] = QLocalSocket,
        timeout_ms: int = 250,
        max_frame_bytes: int = 1024 * 1024,
    ):
        super().__init__(socket_factory, timeout_ms, max_frame_bytes)

    def get_state(self, after_revision: int = -1) -> dict:
        return self._chat_exchange("get_chat_state", {
            "afterRevision": int(after_revision),
        })

    def send_message(self, text: str) -> dict:
        return self._chat_exchange("send_message", {"text": str(text)})

    def retry_message(self, message_id: str) -> dict:
        return self._chat_exchange(
            "retry_message", {"messageId": str(message_id)})

    def stop_response(self) -> dict:
        return self._chat_exchange("stop_response", {})

    def _chat_exchange(self, action: str, payload: dict) -> dict:
        try:
            return self._authenticated_exchange(action, payload)
        except OwnerDiaryProtocolError as error:
            if error.code == "CHAT_AUTH_FAILED":
                self.close()
            raise


class AsyncLauncherChatClient(QObject):
    """Serial local requests without blocking the launcher's event loop.

    A single deadline covers each whole response, including fragmented frames.
    Poll requests are coalesced while user operations keep their ordering.
    """

    connected = Signal()
    disconnected = Signal()
    stateReceived = Signal(dict)
    operationFinished = Signal(str, dict)
    operationFailed = Signal(str, str)

    def __init__(self, socket_factory=QLocalSocket, timeout_ms: int = 2000,
                 max_frame_bytes: int = 1024 * 1024, parent=None):
        super().__init__(parent)
        self._socket_factory = socket_factory
        self._timeout_ms = max(50, int(timeout_ms))
        self._max_frame_bytes = min(max(int(max_frame_bytes), 1024), 1024 * 1024)
        self._socket = None
        self._generation = 0
        self._connected = False
        self._connecting = False
        self._capability_token = None
        self._session_token = None
        self._incoming = bytearray()
        self._request = None
        self._operations = deque()
        self._pending_revision = None
        self._deadline = QTimer(self)
        self._deadline.setSingleShot(True)
        self._deadline.timeout.connect(self._on_timeout)

    @property
    def is_connected(self) -> bool:
        return self._connected

    def connect_to_server(self, socket_name: str, capability_token: str) -> None:
        self.close()
        generation = self._generation
        if not socket_name or len(_decode_token(capability_token)) != 32:
            QTimer.singleShot(0, lambda: self._report_invalid_credentials(generation))
            return
        self._connecting = True
        self._capability_token = capability_token
        socket = self._socket_factory()
        self._socket = socket
        socket.setParent(self)
        socket.connected.connect(
            lambda: self._on_connected(socket, generation))
        socket.readyRead.connect(
            lambda: self._on_ready_read(socket, generation))
        socket.disconnected.connect(
            lambda: self._on_disconnected(socket, generation))
        socket.errorOccurred.connect(
            lambda _error: self._on_socket_error(socket, generation))
        self._deadline.start(self._timeout_ms)
        socket.connectToServer(socket_name, QIODeviceBase.ReadWrite)

    def request_state(self, after_revision: int = -1) -> None:
        if not self.is_connected:
            return
        self._pending_revision = int(after_revision)
        self._dispatch_next()

    def send_message(self, text: str) -> None:
        self._queue_operation("send_message", {"text": str(text)})

    def retry_message(self, message_id: str) -> None:
        self._queue_operation("retry_message", {"messageId": str(message_id)})

    def stop_response(self) -> None:
        self._queue_operation("stop_response", {})

    def close(self) -> None:
        self._generation += 1
        self._deadline.stop()
        socket, self._socket = self._socket, None
        self._connected = False
        self._connecting = False
        self._capability_token = None
        self._session_token = None
        self._request = None
        self._operations.clear()
        self._pending_revision = None
        self._incoming.clear()
        if socket is not None:
            # Abort without a blocking wait; queued callbacks belong to the old generation.
            socket.abort()
            socket.deleteLater()

    def _is_current(self, socket, generation: int) -> bool:
        return socket is self._socket and generation == self._generation

    def _report_invalid_credentials(self, generation: int) -> None:
        if generation == self._generation:
            self.operationFailed.emit("connect", "聊天连接凭据无效")

    def _on_connected(self, socket, generation: int) -> None:
        if not self._is_current(socket, generation):
            return
        # Connection establishment and hello share the original deadline.
        self._send_request("hello", {
            "capabilityToken": self._capability_token,
            "clientNonce": _base64url_token(16),
        }, restart_deadline=False)

    def _queue_operation(self, action: str, payload: dict) -> None:
        if not self.is_connected:
            self.operationFailed.emit(action, "聊天连接已断开")
            return
        # Bound pending UI input while a response is stalled.
        if len(self._operations) >= 32:
            self.operationFailed.emit(action, "待发送操作过多，请稍后再试")
            return
        self._operations.append((action, payload))
        self._dispatch_next()

    def _dispatch_next(self) -> None:
        if not self.is_connected or self._request is not None:
            return
        if self._operations:
            action, payload = self._operations.popleft()
        elif self._pending_revision is not None:
            action = "get_chat_state"
            payload = {"afterRevision": self._pending_revision}
            self._pending_revision = None
        else:
            return
        self._send_request(action, {**payload, "sessionToken": self._session_token})

    def _send_request(self, action: str, payload: dict,
                      restart_deadline: bool = True) -> None:
        socket = self._socket
        if socket is None:
            return
        request_id = str(uuid.uuid4())
        encoded = json.dumps({
            "protocolVersion": 1,
            "requestId": request_id,
            "action": action,
            "payload": payload,
        }, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        if len(encoded) > self._max_frame_bytes:
            self.operationFailed.emit(action, "消息过长，请缩短后重试")
            self._dispatch_next()
            return
        self._request = (action, request_id)
        if restart_deadline:
            self._deadline.start(self._timeout_ms)
        frame = struct.pack(">I", len(encoded)) + encoded
        written = socket.write(frame)
        if socket is self._socket and written != len(frame):
            self._fail_connection("聊天服务连接已中断")

    def _on_ready_read(self, socket, generation: int) -> None:
        if not self._is_current(socket, generation):
            return
        self._incoming.extend(bytes(socket.readAll()))
        while self._is_current(socket, generation) and len(self._incoming) >= 4:
            size = struct.unpack(">I", self._incoming[:4])[0]
            if size == 0 or size > self._max_frame_bytes:
                self._fail_connection("聊天服务返回了无效的数据")
                return
            if len(self._incoming) < size + 4:
                return
            raw = bytes(self._incoming[4:size + 4])
            del self._incoming[:size + 4]
            self._accept_response(raw)

    def _accept_response(self, raw: bytes) -> None:
        try:
            response = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            self._fail_connection("聊天服务返回了无效的数据")
            return
        if self._request is None or not isinstance(response, dict) \
                or response.get("protocolVersion") != 1 \
                or response.get("requestId") != self._request[1]:
            self._fail_connection("聊天服务响应与请求不匹配")
            return
        action = self._request[0]
        if not response.get("ok"):
            error = response.get("error")
            error = error if isinstance(error, dict) else {}
            message = str(error.get("message") or "聊天请求失败")
            if action == "hello" or error.get("code") in {
                    "CHAT_AUTH_FAILED", "OWNER_AUTH_FAILED", "PROTOCOL_INVALID",
                    "PROTOCOL_VERSION_UNSUPPORTED"}:
                self._fail_connection(message)
                return
            self._request = None
            self._deadline.stop()
            self.operationFailed.emit(action, message)
            self._dispatch_next()
            return
        data = response.get("data")
        if not isinstance(data, dict):
            self._fail_connection("聊天服务返回了无效的数据")
            return
        if action == "hello":
            token = data.get("sessionToken", "")
            if not isinstance(token, str) or len(_decode_token(token)) != 32 \
                    or data.get("serverVersion") != 1:
                self._fail_connection("聊天服务握手失败")
                return
            self._session_token = token
            self._capability_token = None
            self._connecting = False
            self._connected = True
        self._request = None
        self._deadline.stop()
        if action == "hello":
            self.connected.emit()
        elif action == "get_chat_state":
            self.stateReceived.emit(data)
        else:
            self.operationFinished.emit(action, data)
        self._dispatch_next()

    def _on_socket_error(self, socket, generation: int) -> None:
        if self._is_current(socket, generation):
            self._fail_connection("无法连接聊天服务" if self._connecting
                                  else "聊天服务连接已中断")

    def _on_disconnected(self, socket, generation: int) -> None:
        if self._is_current(socket, generation):
            self._fail_connection("聊天连接已断开")

    def _on_timeout(self) -> None:
        self._fail_connection("聊天服务响应超时，请稍后重试")

    def _fail_connection(self, message: str) -> None:
        was_connected = self._connected
        action = "connect" if self._connecting else (
            self._request[0] if self._request is not None else "")
        pending_actions = [operation[0] for operation in self._operations]
        self.close()
        if action:
            self.operationFailed.emit(action, message)
        for pending_action in pending_actions:
            self.operationFailed.emit(pending_action, message)
        if was_connected:
            self.disconnected.emit()
