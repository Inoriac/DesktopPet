"""Provider-specific, non-streaming connection probes for launcher editors."""

from __future__ import annotations

import json
import re
from typing import Any
from urllib.parse import urlsplit, urlunsplit

from PySide6.QtCore import QCoreApplication, QObject, QTimer, QUrl, Signal, Slot
from PySide6.QtNetwork import (
    QNetworkAccessManager,
    QNetworkReply,
    QNetworkRequest,
)

from app_state import DEFAULT_ANTHROPIC_VERSION, ModelEndpointState


SUPPORTED_PROVIDERS = ("openai-compatible", "anthropic-messages")
CONNECTION_TIMEOUT_MS = 10_000
MAX_ERROR_CHARS = 400
MAX_RESPONSE_BYTES = 64 * 1024


def _endpoint_url(base_url: str, provider: str) -> str:
    parsed = urlsplit(base_url.strip())
    path = parsed.path.rstrip("/")
    if provider == "anthropic-messages":
        if path.endswith("/messages"):
            pass
        elif path.endswith("/v1"):
            path += "/messages"
        else:
            path += "/v1/messages"
    elif not path.endswith("/chat/completions"):
        path += "/chat/completions"
    return urlunsplit((parsed.scheme, parsed.netloc, path, "", ""))


def _validation_error(endpoint: ModelEndpointState, model: str) -> str | None:
    if endpoint.provider not in SUPPORTED_PROVIDERS:
        return f"不支持的服务协议: {endpoint.provider or '(空)'}"
    try:
        parsed = urlsplit(endpoint.base_url.strip())
        parsed.port  # Validate an explicitly supplied port as well as the host.
    except ValueError:
        return "Base URL 必须是有效的 HTTP(S) 地址"
    if parsed.scheme not in ("http", "https") or not parsed.netloc:
        return "Base URL 必须是有效的 HTTP(S) 地址"
    if not parsed.hostname or parsed.username or parsed.password:
        return "Base URL 必须包含主机名，认证信息请填写到 API Key"
    if not endpoint.api_key:
        return "API Key 不能为空"
    if not model:
        return "模型 ID 不能为空"
    for name, value in endpoint.extra_headers.items():
        if (not re.fullmatch(r"[!#$%&'*+.^_`|~0-9A-Za-z-]+", name)
                or "\r" in value or "\n" in value):
            return "额外请求头的名称或内容无效"
    return None


def _provider_message(body: bytes, fallback: str) -> str:
    try:
        payload = json.loads(body.decode("utf-8", errors="replace"))
    except (json.JSONDecodeError, UnicodeDecodeError):
        text = body.decode("utf-8", errors="replace").strip()
        return text or fallback
    if isinstance(payload, dict):
        error = payload.get("error")
        if isinstance(error, dict) and isinstance(error.get("message"), str):
            return error["message"]
        if isinstance(error, str):
            return error
        if isinstance(payload.get("message"), str):
            return payload["message"]
    return fallback


def _sanitize_message(message: str, api_key: str) -> str:
    sanitized = message
    if api_key:
        sanitized = sanitized.replace(api_key, "[REDACTED]")
    sanitized = re.sub(
        r"(?im)\b(authorization|x-api-key)\s*[:=]\s*[^\r\n]+",
        r"\1: [REDACTED]",
        sanitized,
    )
    sanitized = " ".join(sanitized.split())
    if len(sanitized) > MAX_ERROR_CHARS:
        sanitized = sanitized[: MAX_ERROR_CHARS - 3] + "..."
    return sanitized


def _http_category(status: int) -> str:
    if status in (401, 403):
        return "authentication"
    if status == 404:
        return "endpoint"
    if status == 429:
        return "rate_limit"
    if status >= 500:
        return "provider"
    return "http"


class _ConnectionProbe(QObject):
    """One bounded request; QObject slots disconnect when its owner is deleted."""

    finished = Signal(str, bool, str, str)

    def __init__(self, request_id, reply, api_key, parent=None):
        super().__init__(parent)
        self.request_id = request_id
        self.reply = reply
        self.api_key = api_key
        self._closed = False
        self._body = bytearray()
        self._timer = QTimer(self)
        self._timer.setSingleShot(True)
        self._timer.timeout.connect(self._timeout)
        reply.finished.connect(self._finish)
        reply.readyRead.connect(self._read_available)

    def start(self):
        # QNetworkRequest's transfer timeout resets on incoming data. A probe
        # also needs a wall-clock limit when a server trickles a response.
        self._timer.start(CONNECTION_TIMEOUT_MS)
        if self.reply.isFinished():
            self._finish()

    def cancel(self):
        if self._closed:
            return
        self._closed = True
        self._timer.stop()
        self.reply.abort()
        self.reply.deleteLater()
        self.deleteLater()

    def _complete(self, success, category, message):
        self.cancel()
        self.finished.emit(self.request_id, success, category, message)

    @Slot()
    def _timeout(self):
        if not self._closed:
            self._complete(False, "timeout", "连接测试超时，请检查服务地址或稍后重试")

    @Slot()
    def _read_available(self):
        if self._closed:
            return
        remaining = MAX_RESPONSE_BYTES + 1 - len(self._body)
        self._body.extend(bytes(self.reply.read(remaining)))
        if len(self._body) > MAX_RESPONSE_BYTES:
            self._complete(False, "protocol", "服务响应过大，无法完成连接测试")

    @Slot()
    def _finish(self):
        if self._closed:
            return
        self._read_available()
        if self._closed:
            return
        body = bytes(self._body)
        status_value = self.reply.attribute(QNetworkRequest.HttpStatusCodeAttribute)
        status = int(status_value) if status_value is not None else 0
        if status and not 200 <= status < 300:
            fallback = f"服务返回 HTTP {status}"
            message = _sanitize_message(_provider_message(body, fallback), self.api_key)
            self._complete(False, _http_category(status), message)
        elif self.reply.error() != QNetworkReply.NetworkError.NoError:
            message = _sanitize_message(
                self.reply.errorString() or "网络请求失败", self.api_key)
            self._complete(False, "network", message)
        elif 200 <= status < 300:
            self._complete(True, "success", "连接成功")
        else:
            message = _sanitize_message(
                _provider_message(body, "服务返回了无效响应"), self.api_key)
            self._complete(False, "protocol", message)


class ApiConnectionTester(QObject):
    finished = Signal(str, bool, str, str)

    def __init__(self, parent=None, network_manager: Any = None):
        super().__init__(parent)
        self._network_manager = network_manager or QNetworkAccessManager(self)
        self._pending: dict[str, _ConnectionProbe] = {}
        app = QCoreApplication.instance()
        if app is not None:
            app.aboutToQuit.connect(self.cancel_all)

    def test(
        self, request_id: str, endpoint: ModelEndpointState, model: str
    ) -> None:
        self.cancel(request_id)
        error = _validation_error(endpoint, model)
        if error is not None:
            self.finished.emit(request_id, False, "invalid", error)
            return

        request = QNetworkRequest(QUrl(_endpoint_url(
            endpoint.base_url, endpoint.provider)))
        request.setRawHeader(b"content-type", b"application/json")
        if hasattr(request, "setTransferTimeout"):
            request.setTransferTimeout(CONNECTION_TIMEOUT_MS)

        body = {
            "model": model,
            "max_tokens": 1,
            "stream": False,
            "messages": [{"role": "user", "content": "ping"}],
        }
        if endpoint.provider == "anthropic-messages":
            anthropic_version = (
                endpoint.anthropic_version.strip() or
                DEFAULT_ANTHROPIC_VERSION)
            request.setRawHeader(b"x-api-key", endpoint.api_key.encode("utf-8"))
            request.setRawHeader(
                b"anthropic-version",
                anthropic_version.encode("utf-8"),
            )
        else:
            request.setRawHeader(
                b"Authorization",
                f"Bearer {endpoint.api_key}".encode("utf-8"),
            )
        for name, value in endpoint.extra_headers.items():
            if name.lower() in (
                    "authorization", "content-type", "x-api-key",
                    "anthropic-version"):
                continue
            request.setRawHeader(name.encode("latin-1"), value.encode("utf-8"))

        reply = self._network_manager.post(
            request,
            json.dumps(body, separators=(",", ":")).encode("utf-8"),
        )
        probe = _ConnectionProbe(request_id, reply, endpoint.api_key, self)
        self._pending[request_id] = probe
        probe.finished.connect(self._finish)
        probe.start()

    def cancel(self, request_id: str) -> None:
        probe = self._pending.pop(request_id, None)
        if probe is not None:
            probe.cancel()

    @Slot()
    def cancel_all(self) -> None:
        for request_id in list(self._pending):
            self.cancel(request_id)

    @Slot(str, bool, str, str)
    def _finish(self, request_id, success, category, message) -> None:
        if self._pending.pop(request_id, None) is not None:
            self.finished.emit(request_id, success, category, message)
