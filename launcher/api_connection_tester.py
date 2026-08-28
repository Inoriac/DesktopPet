"""Provider-specific, non-streaming connection probes for launcher editors."""

from __future__ import annotations

import json
import re
from typing import Any
from urllib.parse import urlsplit, urlunsplit

from PySide6.QtCore import QObject, QUrl, Signal
from PySide6.QtNetwork import (
    QNetworkAccessManager,
    QNetworkReply,
    QNetworkRequest,
)

from app_state import ModelEndpointState


SUPPORTED_PROVIDERS = ("openai-compatible", "anthropic-messages")
CONNECTION_TIMEOUT_MS = 10_000
MAX_ERROR_CHARS = 400


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
    parsed = urlsplit(endpoint.base_url.strip())
    if parsed.scheme not in ("http", "https") or not parsed.netloc:
        return "Base URL 必须是有效的 HTTP(S) 地址"
    if not endpoint.api_key:
        return "API Key 不能为空"
    if not model:
        return "模型 ID 不能为空"
    if endpoint.provider == "anthropic-messages" and not endpoint.anthropic_version:
        return "Anthropic Version 不能为空"
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


class ApiConnectionTester(QObject):
    finished = Signal(str, bool, str, str)

    def __init__(self, parent=None, network_manager: Any = None):
        super().__init__(parent)
        self._network_manager = network_manager or QNetworkAccessManager(self)
        self._pending: dict[str, tuple[Any, str]] = {}

    def test(
        self, request_id: str, endpoint: ModelEndpointState, model: str
    ) -> None:
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
            request.setRawHeader(b"x-api-key", endpoint.api_key.encode("utf-8"))
            request.setRawHeader(
                b"anthropic-version",
                endpoint.anthropic_version.encode("utf-8"),
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
        self._pending[request_id] = (reply, endpoint.api_key)
        reply.finished.connect(
            lambda active_request_id=request_id:
            self._finish(active_request_id))

    def _finish(self, request_id: str) -> None:
        pending = self._pending.pop(request_id, None)
        if pending is None:
            return
        reply, api_key = pending
        try:
            body = bytes(reply.readAll())
            status_value = reply.attribute(
                QNetworkRequest.HttpStatusCodeAttribute)
            status = int(status_value) if status_value is not None else 0
            if 200 <= status < 300:
                self.finished.emit(
                    request_id, True, "success", "连接成功")
                return

            if status:
                fallback = f"服务返回 HTTP {status}"
                message = _provider_message(body, fallback)
                message = _sanitize_message(message, api_key)
                self.finished.emit(
                    request_id, False, _http_category(status), message)
                return

            no_error = QNetworkReply.NetworkError.NoError
            if reply.error() != no_error:
                message = _sanitize_message(
                    reply.errorString() or "网络请求失败", api_key)
                self.finished.emit(
                    request_id, False, "network", message)
                return

            message = _sanitize_message(
                _provider_message(body, "服务返回了无效响应"),
                api_key,
            )
            self.finished.emit(request_id, False, "protocol", message)
        finally:
            reply.deleteLater()
