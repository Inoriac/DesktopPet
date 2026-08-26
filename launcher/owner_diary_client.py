"""Authenticated read-only client for the C++ OwnerDiaryServer."""

from __future__ import annotations

import base64
import json
import secrets
import struct
import uuid
from typing import Callable

from PySide6.QtCore import QIODeviceBase, QObject, Signal
from PySide6.QtNetwork import QLocalSocket


PROTOCOL_VERSION = 1
DEFAULT_MAX_FRAME_BYTES = 65536


class OwnerDiaryError(RuntimeError):
    pass


class OwnerDiaryOfflineError(OwnerDiaryError):
    pass


class OwnerDiaryProtocolError(OwnerDiaryError):
    def __init__(self, code: str, message: str, retryable: bool = False):
        super().__init__(message or code)
        self.code = code
        self.retryable = retryable


def _base64url_token(size: int) -> str:
    return base64.urlsafe_b64encode(secrets.token_bytes(size)).rstrip(b"=").decode("ascii")


def _decode_token(value: str) -> bytes:
    if not value or "=" in value:
        return b""
    try:
        padding = "=" * ((4 - len(value) % 4) % 4)
        decoded = base64.b64decode(value + padding, altchars=b"-_", validate=True)
    except (ValueError, TypeError):
        return b""
    canonical = base64.urlsafe_b64encode(decoded).rstrip(b"=").decode("ascii")
    return decoded if canonical == value else b""


class OwnerDiaryClient(QObject):
    disconnected = Signal()

    def __init__(
        self,
        socket_factory: Callable[[], QLocalSocket] = QLocalSocket,
        timeout_ms: int = 1000,
        max_frame_bytes: int = DEFAULT_MAX_FRAME_BYTES,
    ):
        super().__init__()
        self._socket_factory = socket_factory
        self._timeout_ms = max(50, timeout_ms)
        self._max_frame_bytes = min(max(max_frame_bytes, 1024), 1024 * 1024)
        self._socket = None
        self._socket_name: str | None = None
        self._capability_token: str | None = None
        self._session_token: str | None = None

    @property
    def socket_name(self) -> str | None:
        return self._socket_name

    @property
    def capability_token(self) -> str | None:
        return self._capability_token

    @property
    def session_token(self) -> str | None:
        return self._session_token

    @property
    def is_connected(self) -> bool:
        if self._socket is None or self._session_token is None:
            return False
        state_enum = getattr(QLocalSocket, "LocalSocketState", QLocalSocket)
        connected = state_enum.ConnectedState
        return self._socket.state() == connected

    def connect_to_server(self, socket_name: str, capability_token: str) -> None:
        if not socket_name or len(_decode_token(capability_token)) != 32:
            raise OwnerDiaryProtocolError(
                "OWNER_AUTH_FAILED", "Owner diary credentials are invalid")
        self.close()
        self._socket_name = socket_name
        self._capability_token = capability_token
        self._socket = self._socket_factory()
        disconnected = getattr(self._socket, "disconnected", None)
        if disconnected is not None and hasattr(disconnected, "connect"):
            disconnected.connect(
                lambda socket=self._socket: self._handle_socket_disconnected(socket))
        try:
            self._socket.connectToServer(socket_name, QIODeviceBase.ReadWrite)
            if not self._socket.waitForConnected(self._timeout_ms):
                raise OwnerDiaryOfflineError("Owner diary service is offline")
            response = self._exchange("hello", {
                "capabilityToken": capability_token,
                "clientNonce": _base64url_token(16),
            })
            session_token = response.get("sessionToken", "")
            if len(_decode_token(session_token)) != 32 or response.get("serverVersion") != 1:
                raise OwnerDiaryProtocolError(
                    "PROTOCOL_INVALID", "Owner diary hello response is invalid")
            self._session_token = session_token
            self._capability_token = None
        except Exception:
            self._reset_transport()
            raise

    def list_entries(
        self,
        from_date: str,
        to_date: str,
        cursor: str | None,
        limit: int,
    ) -> dict:
        return self._authenticated_exchange("list_diary_entries", {
            "from": from_date,
            "to": to_date,
            "cursor": cursor or "",
            "limit": min(max(int(limit), 1), 100),
        })

    def get_entry(self, entry_id: str) -> dict:
        if not entry_id:
            raise OwnerDiaryProtocolError(
                "DIARY_ENTRY_NOT_FOUND", "Diary entry is unavailable")
        return self._authenticated_exchange(
            "get_diary_entry", {"entryId": entry_id})

    def close(self) -> None:
        self._reset_transport()

    def _authenticated_exchange(self, action: str, payload: dict) -> dict:
        if not self.is_connected or self._session_token is None:
            raise OwnerDiaryOfflineError("Owner diary service is offline")
        request_payload = dict(payload)
        request_payload["sessionToken"] = self._session_token
        try:
            return self._exchange(action, request_payload)
        except OwnerDiaryOfflineError:
            self._reset_transport()
            raise
        except OwnerDiaryProtocolError as error:
            if error.code in {
                    "OWNER_AUTH_FAILED", "PROTOCOL_INVALID",
                    "PROTOCOL_VERSION_UNSUPPORTED"}:
                self._reset_transport()
            raise
        except Exception:
            self._reset_transport()
            raise

    def _exchange(self, action: str, payload: dict) -> dict:
        if self._socket is None:
            raise OwnerDiaryOfflineError("Owner diary service is offline")
        request_id = str(uuid.uuid4())
        request = {
            "protocolVersion": PROTOCOL_VERSION,
            "requestId": request_id,
            "action": action,
            "payload": payload,
        }
        encoded = json.dumps(
            request, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        if not encoded or len(encoded) > self._max_frame_bytes:
            raise OwnerDiaryProtocolError(
                "PROTOCOL_INVALID", "Owner diary request is too large")
        frame = struct.pack(">I", len(encoded)) + encoded
        if self._socket.write(frame) != len(frame) \
                or not self._socket.waitForBytesWritten(self._timeout_ms):
            raise OwnerDiaryOfflineError("Owner diary service disconnected")

        prefix = self._read_exact(4)
        size = struct.unpack(">I", prefix)[0]
        if size == 0 or size > self._max_frame_bytes:
            raise OwnerDiaryProtocolError(
                "PROTOCOL_INVALID", "Owner diary response frame is invalid")
        raw = self._read_exact(size)
        try:
            response = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise OwnerDiaryProtocolError(
                "PROTOCOL_INVALID", "Owner diary response is invalid") from error
        if not isinstance(response, dict) \
                or response.get("protocolVersion") != PROTOCOL_VERSION \
                or response.get("requestId") != request_id:
            raise OwnerDiaryProtocolError(
                "PROTOCOL_INVALID", "Owner diary response envelope is invalid")
        if not response.get("ok"):
            error = response.get("error") if isinstance(response.get("error"), dict) else {}
            raise OwnerDiaryProtocolError(
                str(error.get("code") or "OWNER_DIARY_FAILED"),
                str(error.get("message") or "Owner diary request failed"),
                bool(error.get("retryable")),
            )
        data = response.get("data")
        if not isinstance(data, dict):
            raise OwnerDiaryProtocolError(
                "PROTOCOL_INVALID", "Owner diary response data is invalid")
        return data

    def _read_exact(self, size: int) -> bytes:
        chunks = bytearray()
        while len(chunks) < size:
            if self._socket.bytesAvailable() == 0 \
                    and not self._socket.waitForReadyRead(self._timeout_ms):
                raise OwnerDiaryOfflineError("Owner diary service disconnected")
            chunk = bytes(self._socket.read(size - len(chunks)))
            if not chunk:
                raise OwnerDiaryOfflineError("Owner diary service disconnected")
            chunks.extend(chunk)
        return bytes(chunks)

    def _reset_transport(self) -> None:
        session = self._session_token
        capability = self._capability_token
        socket = self._socket
        self._session_token = None
        self._capability_token = None
        self._socket_name = None
        self._socket = None
        if socket is not None:
            state_enum = getattr(QLocalSocket, "LocalSocketState", QLocalSocket)
            unconnected = state_enum.UnconnectedState
            try:
                if socket.state() != unconnected:
                    socket.disconnectFromServer()
                    if socket.state() != unconnected:
                        socket.waitForDisconnected(100)
            except Exception:
                pass
            try:
                socket.abort()
            except Exception:
                pass
        del session, capability

    def _handle_socket_disconnected(self, socket) -> None:
        if socket is not self._socket:
            return
        self._session_token = None
        self._capability_token = None
        self._socket_name = None
        self._socket = None
        try:
            socket.abort()
        except Exception:
            pass
        self.disconnected.emit()
