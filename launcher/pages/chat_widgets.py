"""Small widgets used by the launcher's private conversation page."""

from __future__ import annotations

from PySide6.QtCore import QDateTime, Qt
from PySide6.QtWidgets import QFrame, QHBoxLayout, QLabel, QSizePolicy, QVBoxLayout, QWidget
from qfluentwidgets import isDarkTheme


def message_datetime(message: dict) -> QDateTime:
    return QDateTime.fromString(str(message.get("timestamp") or ""), Qt.ISODateWithMs).toLocalTime()


class MessageRow(QWidget):
    def __init__(self, message: dict, parent=None):
        super().__init__(parent)
        self.message = {}
        self._available_width = 640
        self._sender_name = "桌宠"
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
        self.text_label = QLabel(self.bubble)
        self.text_label.setTextFormat(Qt.PlainText)
        self.text_label.setWordWrap(True)
        self.text_label.setTextInteractionFlags(Qt.TextSelectableByMouse)
        self.text_label.setSizePolicy(QSizePolicy.Preferred, QSizePolicy.Minimum)
        font = self.text_label.font()
        font.setPointSizeF(11)
        self.text_label.setFont(font)
        layout.addWidget(self.text_label)
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
        text = str(message.get("content") or "") or "正在思考…"
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
        bubble_width = min(bubble_limit, max(64, text_width + 30))
        self.bubble.setFixedWidth(bubble_width)
        height = max(metrics.height(), self.text_label.heightForWidth(bubble_width - 28)) + 22
        self.bubble.setFixedHeight(height)
        time_height = self.time_label.sizeHint().height() + 12 if self.time_label.text() else 0
        self.setFixedHeight(max(height, 34) + time_height)

    def refresh_theme(self) -> None:
        dark = isDarkTheme()
        user = self.message.get("role") == "user"
        bg = "#1687F8" if user else ("#30343D" if dark else "#FFFFFF")
        fg = "#FFFFFF" if user or dark else "#262B35"
        self.bubble.setStyleSheet(
            f"QFrame#messageBubble {{ background: {bg}; border: none; border-radius: 12px; }}"
            f"QLabel {{ color: {fg}; background: transparent; border: none; }}")
        avatar_bg = ("#244B6A" if user else "#474364") if dark else ("#D9EEFF" if user else "#E4DFFA")
        avatar_fg = ("#ABD9FF" if user else "#D5CEFF") if dark else ("#277ABC" if user else "#796AB4")
        self.avatar.setStyleSheet(
            f"background: {avatar_bg}; color: {avatar_fg}; border-radius: 11px; font-weight: 600;")
        self.time_label.setStyleSheet(
            f"color: {'#979EAA' if dark else '#959CA7'}; background: transparent; font-size: 12px;")
