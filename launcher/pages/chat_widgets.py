"""Small widgets used by the launcher's private conversation page."""

from __future__ import annotations

import re

from PySide6.QtCore import QDateTime, Qt, Signal
from PySide6.QtWidgets import QFrame, QHBoxLayout, QLabel, QSizePolicy, QVBoxLayout, QWidget
from qfluentwidgets import PushButton, isDarkTheme

from .chat_images import MessageImageGrid


def message_datetime(message: dict) -> QDateTime:
    return QDateTime.fromString(str(message.get("timestamp") or ""), Qt.ISODateWithMs).toLocalTime()


def concise_error_reason(detail: str) -> str:
    raw = " ".join(str(detail or "").split()).strip()
    if not raw:
        return ""
    lower = raw.lower()
    http_match = re.search(r"\bhttp\s*(\d{3})\b", lower)
    status_code = int(http_match.group(1)) if http_match else 0
    tail = raw.split(":", 1)[1].strip() if ":" in raw else raw
    if len(tail) > 72:
        tail = tail[:69] + "..."
    if status_code in (401, 403) or any(
            k in lower for k in ("unauthorized", "authentication", "invalid api key", "api_key")):
        prefix = f"HTTP {status_code} 鉴权失败" if status_code else "鉴权失败"
        return f"{prefix} ({tail})" if tail and tail.lower() != prefix.lower() else prefix
    if status_code == 404:
        return f"HTTP 404 接口或模型不存在 ({tail})" if tail else "HTTP 404 接口或模型不存在"
    if status_code == 429:
        return f"HTTP 429 请求限流或额度不足 ({tail})" if tail else "HTTP 429 请求限流或额度不足"
    if status_code >= 500:
        return f"HTTP {status_code} 服务端错误 ({tail})" if tail else f"HTTP {status_code} 服务端错误"
    if ("operation canceled" in lower or "timeout" in lower
            or "timed out" in lower or "超时" in lower):
        return f"请求超时 ({tail})" if tail else "请求超时"
    if any(k in lower for k in ("ssl", "tls", "certificate", "handshake")):
        return f"TLS/SSL 握手失败 ({tail})" if tail else "TLS/SSL 握手失败"
    if any(k in lower for k in ("connection", "network", "连接", "网络", "host not found", "refused")):
        return f"网络请求失败 ({tail})" if tail else "网络请求失败"
    return tail or raw[:72]


class MessageRow(QWidget):
    retryRequested = Signal(str)

    def __init__(self, message: dict, parent=None):
        super().__init__(parent)
        self.message = {}
        self._available_width = 640
        self._sender_name = "桌宠"
        self.is_highlighted = False
        self.row_layout = QVBoxLayout(self)
        self.row_layout.setContentsMargins(0, 0, 0, 0)
        self.row_layout.setSpacing(12)
        self.time_label = QLabel(self)
        self.time_label.setAlignment(Qt.AlignCenter)
        self.time_label.setTextFormat(Qt.PlainText)
        self.time_label.hide()
        self.row_layout.addWidget(self.time_label)
        line = QHBoxLayout()
        line.setContentsMargins(0, 0, 0, 0)
        line.setSpacing(10)
        self.avatar = QLabel(self)
        self.avatar.setAlignment(Qt.AlignCenter)
        self.avatar.setFixedSize(34, 34)
        self.bubble = QFrame(self)
        self.bubble.setObjectName("messageBubble")
        layout = QVBoxLayout(self.bubble)
        layout.setContentsMargins(14, 11, 14, 11)
        layout.setSpacing(8)
        self.text_label = QLabel(self.bubble)
        self.text_label.setTextFormat(Qt.PlainText)
        self.text_label.setWordWrap(True)
        self.text_label.setTextInteractionFlags(Qt.TextSelectableByMouse)
        self.text_label.setSizePolicy(QSizePolicy.Preferred, QSizePolicy.Minimum)
        font = self.text_label.font()
        font.setPointSizeF(11)
        self.text_label.setFont(font)
        layout.addWidget(self.text_label)
        self.image_grid = MessageImageGrid(self.bubble)
        layout.addWidget(self.image_grid)
        self.status_bar = QWidget(self.bubble)
        status_layout = QHBoxLayout(self.status_bar)
        status_layout.setContentsMargins(0, 0, 0, 0)
        status_layout.setSpacing(8)
        self.status_label = QLabel(self.status_bar)
        self.status_label.setTextFormat(Qt.PlainText)
        status_layout.addWidget(self.status_label)
        self.retry_button = PushButton("重试", self.status_bar)
        self.retry_button.clicked.connect(lambda: self.retryRequested.emit(str(self.message["id"])))
        status_layout.addWidget(self.retry_button)
        status_layout.addStretch()
        layout.addWidget(self.status_bar)
        if message.get("role") == "user":
            line.addStretch(1)
            line.addWidget(self.bubble, 0, Qt.AlignTop)
            line.addWidget(self.avatar, 0, Qt.AlignTop)
        else:
            line.addWidget(self.avatar, 0, Qt.AlignTop)
            line.addWidget(self.bubble, 0, Qt.AlignTop)
            line.addStretch(1)
        self.row_layout.addLayout(line)
        self.update_message(message)

    def set_sender_name(self, name: str) -> None:
        self._sender_name = name
        self.avatar.setText("我" if self.message.get("role") == "user" else name[:1].upper())
        self.avatar.setToolTip("我" if self.message.get("role") == "user" else name)

    def update_message(self, message: dict) -> None:
        if self.message == message:
            return
        self.message = dict(message)
        images = [image for image in message.get("images", []) if isinstance(image, dict)]
        self.image_grid.set_images(images)
        status = str(message.get("status") or "")
        error_detail = str(message.get("errorMessage") or "").strip()
        base_status = {"failed": "回复失败", "interrupted": "回复中断", "stopped": "已停止回复"}.get(status, "") \
            if message.get("role") == "assistant" else ""
        brief_reason = concise_error_reason(error_detail) if base_status and status in {"failed", "interrupted"} else ""
        status_text = f"{base_status}：{brief_reason}" if brief_reason else base_status
        self.status_label.setText(status_text)
        self.status_label.setToolTip(error_detail if error_detail else "")
        self.status_bar.setVisible(bool(status_text))
        self.retry_button.setVisible(bool(status_text) and status in {"failed", "interrupted"}
                                     and bool(message.get("replyToId")))
        text = str(message.get("content") or "")
        if not text and not images and status in {"pending", "streaming"}:
            text = "正在思考…"
        self.text_label.setVisible(bool(text))
        # A status/timestamp change must not clear the user's text selection.
        if self.text_label.text() != text:
            self.text_label.setText(text)
        self.set_sender_name(self._sender_name)
        self.set_available_width(self._available_width)
        self.refresh_theme()

    def set_time_label(self, text: str) -> None:
        self.time_label.setText(text)
        self.time_label.setVisible(bool(text))
        self.set_available_width(self._available_width)

    def set_available_width(self, width: int) -> None:
        self._available_width = width
        metrics = self.text_label.fontMetrics()
        text_width = max((metrics.horizontalAdvance(line)
                          for line in self.text_label.text().splitlines()), default=32)
        bubble_limit = max(60, min(620, int(width * 0.76), width - 54))
        image_width = (312 if len(self.image_grid.images) == 1 else 388) if self.image_grid.images else 0
        status_width = self.status_bar.sizeHint().width() + 28 if self.status_label.text() else 0
        bubble_width = min(bubble_limit, max(64, text_width + 34, image_width, status_width))
        self.bubble.setFixedWidth(bubble_width)
        text_height = max(metrics.height(), self.text_label.heightForWidth(bubble_width - 32)) \
            if self.text_label.text() else 0
        image_height = self.image_grid.fit(bubble_width - 32)
        status_height = self.status_bar.sizeHint().height() if self.status_label.text() else 0
        sections = [height for height in (text_height, image_height, status_height) if height]
        height = sum(sections) + max(0, len(sections) - 1) * 8 + 26
        self.bubble.setFixedHeight(height)
        time_height = self.time_label.sizeHint().height() + 12 if self.time_label.text() else 0
        self.setFixedHeight(max(height, 34) + time_height)

    def refresh_theme(self) -> None:
        dark = isDarkTheme()
        user = self.message.get("role") == "user"
        bg = "#1687F8" if user else ("#30343D" if dark else "#FFFFFF")
        fg = "#FFFFFF" if user or dark else "#262B35"
        for thumbnail in self.image_grid.thumbnails:
            thumbnail.refresh_theme(fg)
        # Reserve the same border space in both states so highlighting cannot
        # shrink the text area and clip a wrapped line.
        border = "2px solid #FFBE54" if self.is_highlighted else "2px solid transparent"
        self.bubble.setStyleSheet(
            f"QFrame#messageBubble {{ background: {bg}; border: {border}; border-radius: 12px; }}"
            f"QLabel {{ color: {fg}; background: transparent; border: none; }}")
        avatar_bg = ("#244B6A" if user else "#474364") if dark else ("#D9EEFF" if user else "#E4DFFA")
        avatar_fg = ("#ABD9FF" if user else "#D5CEFF") if dark else ("#277ABC" if user else "#796AB4")
        self.avatar.setStyleSheet(
            f"background: {avatar_bg}; color: {avatar_fg}; border-radius: 11px; font-weight: 600;")
        self.time_label.setStyleSheet(
            f"color: {'#979EAA' if dark else '#959CA7'}; background: transparent; font-size: 12px;")

    def set_highlighted(self, highlighted: bool) -> None:
        self.is_highlighted = highlighted
        self.refresh_theme()
