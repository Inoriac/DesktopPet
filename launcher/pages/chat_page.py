"""Launcher-native chat page backed by the C++ AI runtime."""

from __future__ import annotations

from PySide6.QtCore import Qt, QTimer, Signal
from PySide6.QtGui import QKeyEvent
from PySide6.QtWidgets import (
    QFrame,
    QHBoxLayout,
    QLabel,
    QPlainTextEdit,
    QScrollArea,
    QSizePolicy,
    QVBoxLayout,
    QWidget,
)
from qfluentwidgets import (
    BodyLabel,
    CaptionLabel,
    FluentIcon as FIF,
    StrongBodyLabel,
    ToolButton,
    isDarkTheme,
)

from launcher_chat_client import LauncherChatClient, LauncherChatError


class ChatInput(QPlainTextEdit):
    submitRequested = Signal()

    def keyPressEvent(self, event: QKeyEvent) -> None:
        if event.key() in (Qt.Key_Return, Qt.Key_Enter) \
                and not (event.modifiers() & Qt.ShiftModifier):
            self.submitRequested.emit()
            event.accept()
            return
        super().keyPressEvent(event)


class ChatPage(QWidget):
    openRequested = Signal()
    connectionLost = Signal(str)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setObjectName("ChatPage")
        self._client: LauncherChatClient | None = None
        self._revision = -1
        self._open_request_id = 0
        self._busy = False
        self._ai_enabled = False
        self._messages: list[dict] = []

        root = QVBoxLayout(self)
        root.setContentsMargins(36, 20, 36, 24)
        root.setSpacing(14)

        header = QWidget(self)
        header_layout = QHBoxLayout(header)
        header_layout.setContentsMargins(0, 0, 0, 0)
        header_layout.setSpacing(12)
        self.title_label = StrongBodyLabel("聊天", header)
        self.status_label = CaptionLabel("离线", header)
        header_layout.addWidget(self.title_label)
        header_layout.addWidget(self.status_label)
        header_layout.addStretch(1)
        root.addWidget(header)

        stats = QWidget(self)
        stats_layout = QHBoxLayout(stats)
        stats_layout.setContentsMargins(0, 0, 0, 0)
        stats_layout.setSpacing(24)
        self.call_count_label = CaptionLabel("调用 0", stats)
        self.success_count_label = CaptionLabel("成功 0", stats)
        self.failure_count_label = CaptionLabel("失败 0", stats)
        self.token_count_label = CaptionLabel("Token 0", stats)
        for label in (
                self.call_count_label,
                self.success_count_label,
                self.failure_count_label,
                self.token_count_label):
            stats_layout.addWidget(label)
        stats_layout.addStretch(1)
        root.addWidget(stats)

        divider = QFrame(self)
        divider.setFrameShape(QFrame.HLine)
        divider.setFrameShadow(QFrame.Plain)
        root.addWidget(divider)

        self.message_area = QScrollArea(self)
        self.message_area.setWidgetResizable(True)
        self.message_area.setFrameShape(QFrame.NoFrame)
        self.message_area.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        self.message_area.setStyleSheet(
            "QScrollArea { border: none; background: transparent; }")
        self.message_view = QWidget(self.message_area)
        self.message_layout = QVBoxLayout(self.message_view)
        self.message_layout.setContentsMargins(0, 4, 0, 8)
        self.message_layout.setSpacing(10)
        self.message_layout.addStretch(1)
        self.message_area.setWidget(self.message_view)
        root.addWidget(self.message_area, 1)

        composer = QWidget(self)
        composer_layout = QHBoxLayout(composer)
        composer_layout.setContentsMargins(0, 0, 0, 0)
        composer_layout.setSpacing(10)
        self.input_edit = ChatInput(composer)
        self.input_edit.setPlaceholderText("输入消息")
        self.input_edit.setMinimumHeight(76)
        self.input_edit.setMaximumHeight(110)
        self.input_edit.setSizePolicy(
            QSizePolicy.Expanding, QSizePolicy.Preferred)
        self.input_edit.submitRequested.connect(self._submit_or_stop)
        self.action_button = ToolButton(FIF.SEND, composer)
        self.action_button.setFixedSize(40, 40)
        self.action_button.setToolTip("发送")
        self.action_button.clicked.connect(self._submit_or_stop)
        composer_layout.addWidget(self.input_edit, 1)
        composer_layout.addWidget(
            self.action_button, 0, Qt.AlignRight | Qt.AlignBottom)
        root.addWidget(composer)

        self._poll_timer = QTimer(self)
        self._poll_timer.setInterval(250)
        self._poll_timer.timeout.connect(self.poll_once)
        self._sync_controls()
        self.refresh_theme()

    def set_client(self, client: LauncherChatClient | None) -> None:
        previous = self._client
        if previous is not None:
            try:
                previous.disconnected.disconnect(self._on_disconnected)
            except (RuntimeError, TypeError):
                pass
        self._client = client
        self._revision = -1
        self._open_request_id = 0
        self._busy = False
        self._ai_enabled = False
        if client is not None:
            client.disconnected.connect(self._on_disconnected)
            self._poll_timer.start()
            self.poll_once()
        else:
            self._poll_timer.stop()
            self._set_offline()

    def poll_once(self) -> None:
        if self._client is None:
            self._set_offline()
            return
        if not self._client.is_connected:
            self._set_offline()
            self.connectionLost.emit("聊天连接已断开")
            return
        try:
            state = self._client.get_state(self._revision)
        except LauncherChatError as error:
            if self._client is None or not self._client.is_connected:
                self._set_offline()
                self.connectionLost.emit(str(error) or "聊天连接已断开")
            else:
                self.status_label.setText(str(error) or "状态同步失败")
            return

        revision = int(state.get("revision", self._revision))
        open_request_id = int(state.get("openRequestId", 0))
        should_open = open_request_id > self._open_request_id
        self._open_request_id = max(self._open_request_id, open_request_id)
        self._revision = revision
        if not state.get("unchanged", False):
            self._apply_state(state)
        if should_open:
            self.openRequested.emit()

    def refresh_theme(self) -> None:
        dark = isDarkTheme()
        border = "rgba(255,255,255,0.16)" if dark else "rgba(0,0,0,0.18)"
        text = "#F5F5F5" if dark else "#202020"
        background = "rgba(255,255,255,0.055)" if dark else "rgba(255,255,255,0.78)"
        self.input_edit.setStyleSheet(
            "QPlainTextEdit {"
            f"color: {text}; background: {background}; border: 1px solid {border};"
            "border-radius: 6px; padding: 9px 10px; selection-background-color: #0F6CBD;"
            "}")
        if self._messages:
            self._render_messages(self._messages)

    def _apply_state(self, state: dict) -> None:
        pet_name = str(state.get("petName") or "桌宠")
        self.title_label.setText(f"与 {pet_name} 聊天")
        self._busy = bool(state.get("busy"))
        self._ai_enabled = bool(state.get("aiEnabled"))
        self.status_label.setText(
            "思考中" if self._busy else (
                "已连接" if self._ai_enabled else "已连接 · AI 未启用"))

        statistics = state.get("statistics")
        if not isinstance(statistics, dict):
            statistics = {}
        self.call_count_label.setText(
            f"调用 {self._count(statistics.get('callCount'))}")
        self.success_count_label.setText(
            f"成功 {self._count(statistics.get('successCount'))}")
        self.failure_count_label.setText(
            f"失败 {self._count(statistics.get('failureCount'))}")
        self.token_count_label.setText(
            f"Token {self._count(statistics.get('totalTokens')):,}")

        messages = state.get("messages")
        self._messages = [item for item in messages if isinstance(item, dict)] \
            if isinstance(messages, list) else []
        self._render_messages(self._messages)
        self._sync_controls()

    def _render_messages(self, messages: list[dict]) -> None:
        scrollbar = self.message_area.verticalScrollBar()
        follow_bottom = scrollbar.maximum() - scrollbar.value() <= 48
        previous_position = scrollbar.value()
        while self.message_layout.count() > 1:
            item = self.message_layout.takeAt(0)
            widget = item.widget()
            if widget is not None:
                widget.deleteLater()

        for message in messages:
            self.message_layout.insertWidget(
                self.message_layout.count() - 1,
                self._message_row(message))

        def restore_scroll() -> None:
            if follow_bottom:
                scrollbar.setValue(scrollbar.maximum())
            else:
                scrollbar.setValue(min(previous_position, scrollbar.maximum()))

        QTimer.singleShot(0, restore_scroll)

    def _message_row(self, message: dict) -> QWidget:
        role = str(message.get("role") or "assistant")
        status = str(message.get("status") or "complete")
        content = str(message.get("content") or "")
        error = str(message.get("errorMessage") or "")
        row = QWidget(self.message_view)
        row_layout = QHBoxLayout(row)
        row_layout.setContentsMargins(0, 0, 0, 0)

        bubble = QFrame(row)
        bubble.setMaximumWidth(660)
        bubble_layout = QVBoxLayout(bubble)
        bubble_layout.setContentsMargins(12, 9, 12, 9)
        bubble_layout.setSpacing(4)
        dark = isDarkTheme()
        if role == "user":
            background, foreground = "#0F6CBD", "#FFFFFF"
            row_layout.addStretch(1)
        else:
            background = "rgba(255,255,255,0.09)" if dark \
                else "rgba(0,0,0,0.055)"
            foreground = "#F5F5F5" if dark else "#202020"
        bubble.setStyleSheet(
            "QFrame {"
            f"background: {background}; border: none; border-radius: 6px;"
            "}"
            "QLabel {"
            f"color: {foreground}; background: transparent; border: none;"
            "}")

        if not content and status in {"pending", "streaming"}:
            content = "思考中..."
        text_label = QLabel(content or error or "回复失败", bubble)
        text_label.setWordWrap(True)
        text_label.setTextInteractionFlags(Qt.TextSelectableByMouse)
        text_label.setSizePolicy(QSizePolicy.Preferred, QSizePolicy.Minimum)
        bubble_layout.addWidget(text_label)

        terminal_text = {
            "failed": error or "回复失败",
            "interrupted": error or "回复中断",
            "stopped": "已停止",
        }.get(status, "")
        if terminal_text and terminal_text != content:
            status_row = QWidget(bubble)
            status_layout = QHBoxLayout(status_row)
            status_layout.setContentsMargins(0, 0, 0, 0)
            status_label = CaptionLabel(terminal_text, status_row)
            status_layout.addWidget(status_label)
            status_layout.addStretch(1)
            if status in {"failed", "interrupted"}:
                retry = ToolButton(FIF.SYNC, status_row)
                retry.setFixedSize(30, 30)
                retry.setToolTip("重新生成")
                retry.setEnabled(not self._busy)
                message_id = str(message.get("id") or "")
                retry.clicked.connect(
                    lambda _checked=False, mid=message_id: self._retry(mid))
                status_layout.addWidget(retry)
            bubble_layout.addWidget(status_row)

        row_layout.addWidget(bubble)
        if role != "user":
            row_layout.addStretch(1)
        return row

    def _submit_or_stop(self) -> None:
        if self._client is None or not self._client.is_connected:
            self._set_offline()
            return
        try:
            if self._busy:
                self._client.stop_response()
            else:
                text = self.input_edit.toPlainText().strip()
                if not text or not self._ai_enabled:
                    return
                self._client.send_message(text)
                self.input_edit.clear()
            self.poll_once()
        except LauncherChatError as error:
            if self._client is None or not self._client.is_connected:
                self._set_offline()
                self.connectionLost.emit(str(error) or "聊天连接已断开")
            else:
                self.status_label.setText(str(error) or "发送失败")
            self._sync_controls()

    def _retry(self, message_id: str) -> None:
        if not message_id or self._client is None or self._busy:
            return
        try:
            self._client.retry_message(message_id)
            self.poll_once()
        except LauncherChatError as error:
            if self._client is None or not self._client.is_connected:
                self._set_offline()
                self.connectionLost.emit(str(error) or "聊天连接已断开")
            else:
                self.status_label.setText(str(error) or "重试失败")

    def _on_disconnected(self) -> None:
        self._set_offline()
        self.connectionLost.emit("聊天连接已断开")

    def _set_offline(self) -> None:
        self._poll_timer.stop()
        self._busy = False
        self._ai_enabled = False
        self.status_label.setText("离线")
        self._sync_controls()

    def _sync_controls(self) -> None:
        connected = self._client is not None and self._client.is_connected
        self.input_edit.setEnabled(connected and self._ai_enabled)
        if self._busy and connected:
            self.action_button.setIcon(FIF.CANCEL)
            self.action_button.setToolTip("停止回复")
            self.action_button.setEnabled(True)
        else:
            self.action_button.setIcon(FIF.SEND)
            self.action_button.setToolTip("发送")
            self.action_button.setEnabled(connected and self._ai_enabled)

    @staticmethod
    def _count(value) -> int:
        try:
            return max(0, int(value or 0))
        except (TypeError, ValueError):
            return 0
