"""Launcher chat: bounded history, stable scrolling and asynchronous commands."""

from __future__ import annotations

from PySide6.QtCore import QEvent, Qt, QTimer, Signal
from PySide6.QtGui import QKeyEvent
from PySide6.QtWidgets import (
    QFrame, QHBoxLayout, QLabel, QLayout, QPlainTextEdit, QPushButton, QScrollArea,
    QSizePolicy, QVBoxLayout, QWidget,
)
from qfluentwidgets import (
    CaptionLabel, FluentIcon as FIF, InfoBar, InfoBarPosition,
    StrongBodyLabel, PushButton, PrimaryPushButton, isDarkTheme,
)


class ChatInput(QPlainTextEdit):
    submitRequested = Signal()

    def keyPressEvent(self, event: QKeyEvent) -> None:
        if event.key() in (Qt.Key_Return, Qt.Key_Enter) \
                and not (event.modifiers() & Qt.ShiftModifier):
            self.submitRequested.emit()
            event.accept()
            return
        super().keyPressEvent(event)


class MessageRow(QWidget):
    def __init__(self, message: dict, parent=None):
        super().__init__(parent)
        self.message = {}
        self.row_layout = QHBoxLayout(self)
        self.row_layout.setContentsMargins(4, 0, 4, 0)
        self.bubble = QFrame(self)
        layout = QVBoxLayout(self.bubble)
        layout.setContentsMargins(14, 10, 14, 10)
        self.text_label = QLabel(self.bubble)
        self.text_label.setTextFormat(Qt.PlainText)
        self.text_label.setWordWrap(True)
        self.text_label.setTextInteractionFlags(Qt.TextSelectableByMouse)
        self.text_label.setSizePolicy(QSizePolicy.Preferred, QSizePolicy.Minimum)
        layout.addWidget(self.text_label)
        if message.get("role") == "user":
            self.row_layout.addStretch(1)
        self.row_layout.addWidget(self.bubble)
        if message.get("role") != "user":
            self.row_layout.addStretch(1)
        self._available_width = 640
        self.update_message(message)

    def update_message(self, message: dict) -> None:
        if self.message == message:
            return
        self.message = dict(message)
        content = str(message.get("content") or "")
        self.text_label.setText(content or "正在思考…")
        self.set_available_width(self._available_width)
        self.refresh_theme()

    def set_available_width(self, width: int) -> None:
        self._available_width = width
        metrics = self.text_label.fontMetrics()
        text_width = max((metrics.horizontalAdvance(line)
                          for line in self.text_label.text().splitlines()), default=32)
        bubble_width = min(width, max(70, text_width + 32))
        self.bubble.setFixedWidth(bubble_width)
        # QLabel's preferred size is based on a narrower wrap width. Explicitly
        # use its height at the actual bubble width so QScrollArea cannot retain
        # a large empty tail after the layout wraps fewer lines.
        height = max(metrics.height(), self.text_label.heightForWidth(bubble_width - 28)) + 20
        self.bubble.setFixedHeight(height)
        self.setFixedHeight(height)

    def refresh_theme(self) -> None:
        dark = isDarkTheme()
        user = self.message.get("role") == "user"
        bg = "#2585E6" if user else ("#303238" if dark else "#FFFFFF")
        fg = "#FFFFFF" if user or dark else "#20242B"
        self.bubble.setStyleSheet(
            f"QFrame {{ background: {bg}; border: none; border-radius: 10px; }}"
            f"QLabel {{ color: {fg}; background: transparent; border: none; }}")


class ChatPage(QWidget):
    openRequested = Signal()
    connectionLost = Signal(str)
    PAGE_SIZE = 40

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setObjectName("ChatPage")
        self._client = None
        self._revision = -1
        self._open_request_id = 0
        self._busy = False
        self._ai_enabled = False
        self._messages: list[dict] = []
        self._rows: dict[str, MessageRow] = {}
        self._visible_count = self.PAGE_SIZE
        self._follow_bottom = True
        self._restoring_scroll = False
        self._render_generation = 0
        self._known_statuses: dict[str, str] | None = None
        self._pending_text: str | None = None
        self._send_inflight = False
        self._waiting_to_send = False
        self._last_error = ""
        self._diagnostic_events: list[str] = []

        root = QVBoxLayout(self)
        root.setContentsMargins(24, 20, 24, 20)
        root.setSpacing(12)
        header = QHBoxLayout()
        self.title_label = StrongBodyLabel("聊天", self)
        self.status_label = CaptionLabel("离线", self)
        header.addWidget(self.title_label)
        header.addWidget(self.status_label)
        header.addStretch(1)
        self.debug_button = PushButton("调试信息", self)
        self.debug_button.setCheckable(True)
        self.debug_button.setToolTip("查看调用统计与技术诊断")
        header.addWidget(self.debug_button)
        root.addLayout(header)

        self.message_area = QScrollArea(self)
        self.message_area.setWidgetResizable(True)
        self.message_area.setFrameShape(QFrame.NoFrame)
        self.message_area.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        self.message_area.setVerticalScrollBarPolicy(Qt.ScrollBarAlwaysOn)
        self.message_view = QWidget(self.message_area)
        self.message_layout = QVBoxLayout(self.message_view)
        self.message_layout.setContentsMargins(12, 12, 12, 18)
        self.message_layout.setSpacing(14)
        self.message_layout.setSizeConstraint(QLayout.SetMinAndMaxSize)
        self.history_button = QPushButton("查看更早消息", self.message_view)
        self.history_button.setFlat(True)
        self.history_button.clicked.connect(self._load_older)
        self.message_layout.addWidget(self.history_button, 0, Qt.AlignHCenter)
        self.message_layout.addStretch(1)
        self.message_area.setWidget(self.message_view)
        root.addWidget(self.message_area, 1)
        self.jump_button = QPushButton("↓ 返回底部", self.message_area.viewport())
        self.jump_button.setObjectName("chatJumpToBottom")
        self.jump_button.setToolTip("查看最新消息")
        self.jump_button.setFixedSize(112, 34)
        self.jump_button.clicked.connect(self.scroll_to_bottom)
        self.jump_button.hide()
        self.message_area.viewport().installEventFilter(self)
        scrollbar = self.message_area.verticalScrollBar()
        scrollbar.valueChanged.connect(self._scroll_changed)
        scrollbar.rangeChanged.connect(self._scroll_range_changed)

        self.diagnostics_panel = QWidget(self)
        diagnostics_layout = QVBoxLayout(self.diagnostics_panel)
        diagnostics_layout.setContentsMargins(0, 0, 0, 0)
        stats = QHBoxLayout()
        self.call_count_label = CaptionLabel("调用 0", self)
        self.success_count_label = CaptionLabel("成功 0", self)
        self.failure_count_label = CaptionLabel("失败 0", self)
        self.token_count_label = CaptionLabel("Token 0", self)
        for label in (self.call_count_label, self.success_count_label,
                      self.failure_count_label, self.token_count_label):
            stats.addWidget(label)
        stats.addStretch(1)
        diagnostics_layout.addLayout(stats)
        self.diagnostics_text = QPlainTextEdit(self.diagnostics_panel)
        self.diagnostics_text.setReadOnly(True)
        self.diagnostics_text.setMaximumHeight(110)
        self.diagnostics_text.setPlaceholderText("暂无诊断信息")
        diagnostics_layout.addWidget(self.diagnostics_text)
        self.diagnostics_panel.hide()
        self.debug_button.toggled.connect(self.diagnostics_panel.setVisible)
        root.addWidget(self.diagnostics_panel)

        composer = QVBoxLayout()
        composer.setSpacing(8)
        self.input_edit = ChatInput(self)
        self.input_edit.setPlaceholderText("输入消息，Enter 发送，Shift + Enter 换行")
        self.input_edit.setFixedHeight(92)
        self.input_edit.submitRequested.connect(self._submit)
        composer.addWidget(self.input_edit)
        controls = QHBoxLayout()
        controls.addWidget(CaptionLabel("Enter 发送 · Shift + Enter 换行", self))
        controls.addStretch(1)
        self.stop_button = PushButton("停止回复", self)
        self.stop_button.clicked.connect(self._stop)
        controls.addWidget(self.stop_button)
        self.action_button = PrimaryPushButton("发送", self)
        self.action_button.setMinimumWidth(88)
        self.action_button.setToolTip("发送消息")
        self.action_button.clicked.connect(self._submit)
        controls.addWidget(self.action_button)
        composer.addLayout(controls)
        root.addLayout(composer)

        self._poll_timer = QTimer(self)
        self._poll_timer.setInterval(250)
        self._poll_timer.timeout.connect(self.poll_once)
        self._sync_controls()
        self.refresh_theme()

    def set_client(self, client) -> None:
        if self._client is not None:
            for signal, slot in self._client_connections(self._client):
                try:
                    signal.disconnect(slot)
                except (RuntimeError, TypeError):
                    pass
        self._poll_timer.stop()
        self._client = client
        self._revision = -1
        self._open_request_id = 0
        self._known_statuses = None
        self._pending_text = None
        self._send_inflight = self._waiting_to_send = False
        self._busy = self._ai_enabled = False
        self._visible_count = self.PAGE_SIZE
        self._follow_bottom = True
        self._last_error = ""
        if client is not None:
            # A new profile must not show the previous profile's conversation.
            self._messages = []
            self._render_messages([])
            self._diagnostic_events.clear()
            self._update_diagnostics()
            for signal, slot in self._client_connections(client):
                signal.connect(slot)
            self._poll_timer.start()
            self.poll_once()
        else:
            self._set_offline()

    def _client_connections(self, client):
        return ((client.disconnected, self._on_disconnected),
                (client.stateReceived, self._apply_state),
                (client.operationFinished, self._operation_finished),
                (client.operationFailed, self._operation_failed))

    def poll_once(self) -> None:
        if self._client is not None and self._client.is_connected:
            self._client.request_state(self._revision)

    def _apply_state(self, state: dict) -> None:
        self._revision = int(state.get("revision", self._revision))
        request_id = int(state.get("openRequestId", 0))
        if request_id > self._open_request_id:
            self._open_request_id = request_id
            self.openRequested.emit()
        if state.get("unchanged", False):
            return
        self.title_label.setText(f"与 {state.get('petName') or '桌宠'} 聊天")
        self._busy = bool(state.get("busy"))
        self._ai_enabled = bool(state.get("aiEnabled"))
        self.status_label.setText("正在回复…" if self._busy else (
            "已连接" if self._ai_enabled else "AI 未启用"))
        statistics = state.get("statistics") or {}
        for label, key, title in (
                (self.call_count_label, "callCount", "调用"),
                (self.success_count_label, "successCount", "成功"),
                (self.failure_count_label, "failureCount", "失败"),
                (self.token_count_label, "totalTokens", "Token")):
            label.setText(f"{title} {self._count(statistics.get(key)):,}")
        messages = state.get("messages")
        messages = [m for m in messages if isinstance(m, dict)] \
            if isinstance(messages, list) else []
        previous = self._known_statuses
        self._known_statuses = {str(m.get("id")): str(m.get("status"))
                                for m in messages}
        self._messages = messages
        self._render_messages(messages)
        self._update_diagnostics()
        if previous is not None:
            failures = [m for m in messages
                        if m.get("status") in {"failed", "interrupted"}
                        and previous.get(str(m.get("id"))) != m.get("status")]
            if failures:
                failed = failures[-1]
                self._show_problem("回复未完成", self._friendly_error(
                    str(failed.get("errorMessage") or "")),
                                   str(failed.get("errorMessage") or ""),
                                   str(failed.get("id") or ""))
        self._sync_controls()
        if self._waiting_to_send and not self._busy:
            self._waiting_to_send = False
            self._send_pending()

    @staticmethod
    def _visible_message(message: dict) -> bool:
        if message.get("role") not in {"user", "assistant"}:
            return False
        # Diagnostics belong to the debug panel, not an empty error bubble.
        return bool(message.get("content")) or message.get("status") in {
            "pending", "streaming"}

    def _render_messages(self, messages: list[dict]) -> None:
        scrollbar = self.message_area.verticalScrollBar()
        position = scrollbar.value()
        following = self._follow_bottom
        self._scroll_anchor = None
        if not following:
            for mid, row in sorted(self._rows.items(), key=lambda pair: pair[1].y()):
                if row.y() + row.height() >= position:
                    self._scroll_anchor = (mid, row.y() - position)
                    break
        self._restoring_scroll = True
        visible = [m for m in messages if self._visible_message(m)]
        if not following and self._rows and not getattr(self, "_loading_older", False):
            first_id = min(self._rows, key=lambda mid: self._rows[mid].y())
            first = next((i for i, m in enumerate(visible)
                          if str(m.get("id")) == first_id), None)
            if first is not None:
                self._visible_count = max(self._visible_count, len(visible) - first)
        selected = visible[-self._visible_count:]
        ids = {str(m.get("id")) for m in selected}
        for mid in list(self._rows):
            if mid not in ids:
                row = self._rows.pop(mid)
                self.message_layout.removeWidget(row)
                row.hide()
                row.deleteLater()
        for index, message in enumerate(selected, 1):
            mid = str(message.get("id"))
            row = self._rows.get(mid)
            if row is None:
                row = MessageRow(message, self.message_view)
                self._rows[mid] = row
            else:
                row.update_message(message)
            if self.message_layout.indexOf(row) != index:
                self.message_layout.insertWidget(index, row)
            row.set_available_width(max(180, min(
                640, int(self.message_area.viewport().width() * 0.76))))
        self.history_button.setVisible(len(visible) > self._visible_count)
        self._render_generation += 1
        generation = self._render_generation

        def restore():
            if generation != self._render_generation:
                return
            self.message_layout.activate()
            self._follow_bottom = following
            if following:
                scrollbar.setValue(scrollbar.maximum())
            elif self._scroll_anchor and self._scroll_anchor[0] in self._rows:
                mid, offset = self._scroll_anchor
                scrollbar.setValue(self._rows[mid].y() - offset)
            else:
                scrollbar.setValue(position)
            def settled():
                if generation != self._render_generation:
                    return
                if not following and self._scroll_anchor:
                    mid, offset = self._scroll_anchor
                    if mid in self._rows:
                        scrollbar.setValue(self._rows[mid].y() - offset)
                self._restoring_scroll = False
                self._loading_older = False
                self._scroll_anchor = None
                self._update_jump_button()
            QTimer.singleShot(0, self, settled)
        QTimer.singleShot(0, self, restore)

    def _load_older(self) -> None:
        self._visible_count += self.PAGE_SIZE
        self._loading_older = True
        self._follow_bottom = False
        self._render_messages(self._messages)

    def scroll_to_bottom(self) -> None:
        self._render_generation += 1
        self._restoring_scroll = False
        self._scroll_anchor = None
        self._loading_older = False
        self._follow_bottom = True
        self.message_area.verticalScrollBar().setValue(
            self.message_area.verticalScrollBar().maximum())
        self._update_jump_button()

    def _scroll_changed(self, value: int) -> None:
        if not self._restoring_scroll:
            self._follow_bottom = (
                self.message_area.verticalScrollBar().maximum() - value <= 24)
        self._update_jump_button()

    def _scroll_range_changed(self, _minimum: int, maximum: int) -> None:
        if self._follow_bottom:
            self.message_area.verticalScrollBar().setValue(maximum)
        elif self._restoring_scroll and getattr(self, "_scroll_anchor", None):
            mid, offset = self._scroll_anchor
            if mid in self._rows:
                self.message_area.verticalScrollBar().setValue(self._rows[mid].y() - offset)
        self._update_jump_button()

    def _update_jump_button(self) -> None:
        bar = self.message_area.verticalScrollBar()
        self.jump_button.setVisible(bar.maximum() - bar.value() > 48)
        viewport = self.message_area.viewport()
        self.jump_button.move(max(0, viewport.width() - 130),
                              max(0, viewport.height() - 50))
        self.jump_button.raise_()

    def eventFilter(self, watched, event) -> bool:
        if watched is self.message_area.viewport() and event.type() == QEvent.Resize:
            for row in self._rows.values():
                row.set_available_width(max(180, min(
                    640, int(watched.width() * 0.76))))
            self._update_jump_button()
        return super().eventFilter(watched, event)

    def _submit(self) -> None:
        if self._client is None or not self._client.is_connected \
                or not self._ai_enabled or self._pending_text is not None:
            return
        text = self.input_edit.toPlainText().strip()
        if not text:
            return
        if len(text) > 8000:
            self._show_problem("消息过长", "请将消息缩短到 8000 个字符以内。")
            return
        self._last_error = ""
        self._pending_text = text
        if self._busy:
            self._waiting_to_send = True
            self._client.stop_response()
        else:
            self._send_pending()
        self._sync_controls()

    def _send_pending(self) -> None:
        if self._pending_text is None or self._client is None:
            return
        self._send_inflight = True
        self.scroll_to_bottom()
        self._client.send_message(self._pending_text)
        self._sync_controls()

    def _stop(self) -> None:
        if self._client is not None and self._client.is_connected:
            self._waiting_to_send = False
            if not self._send_inflight:
                self._pending_text = None
            self._client.stop_response()
            self._sync_controls()

    def _operation_finished(self, operation: str, _result: dict) -> None:
        self._last_error = ""
        if operation == "send_message":
            if self.input_edit.toPlainText().strip() == self._pending_text:
                self.input_edit.clear()
            self._pending_text = None
            self._send_inflight = False
            self.scroll_to_bottom()
        self._sync_controls()
        self.poll_once()

    def _operation_failed(self, operation: str, detail: str) -> None:
        if operation in {"send_message", "stop_response"}:
            self._pending_text = None
            self._send_inflight = self._waiting_to_send = False
        title = "发送失败" if operation == "send_message" else "连接暂时不可用"
        self._show_problem(title, "请检查桌宠和模型服务的连接后重试，草稿已保留。", detail)
        self._sync_controls()

    def _retry(self, message_id: str) -> None:
        if self._client is not None and self._client.is_connected and not self._busy:
            self.scroll_to_bottom()
            self._client.retry_message(message_id)

    @staticmethod
    def _friendly_error(detail: str) -> str:
        text = detail.lower()
        if "timeout" in text or "timed out" in text or "超时" in text:
            return "模型服务响应超时，请稍后重试。"
        if any(word in text for word in ("connection", "network", "连接", "网络")):
            return "与模型服务的连接中断，请检查网络后重试。"
        if any(word in text for word in ("401", "403", "unauthorized", "authentication")):
            return "模型服务拒绝访问，请检查连接配置。"
        return "暂时无法完成回复，请稍后重试。"

    def _show_problem(self, title: str, message: str, detail: str = "",
                      retry_id: str = "") -> None:
        if detail:
            self._diagnostic_events.append(f"{title}: {detail}")
            self._diagnostic_events = self._diagnostic_events[-30:]
            self._update_diagnostics()
        key = f"{title}:{detail}:{retry_id}"
        if key == self._last_error:
            return
        self._last_error = key
        notice = InfoBar.error(title, message, parent=self.window(),
                               position=InfoBarPosition.TOP, duration=6000)
        if retry_id:
            retry = QPushButton("重试", notice)
            retry.clicked.connect(lambda: self._retry(retry_id))
            notice.addWidget(retry)

    def _update_diagnostics(self) -> None:
        lines = list(self._diagnostic_events)
        for message in self._messages:
            if message.get("role") not in {"user", "assistant"}:
                lines.append(str(message.get("content") or ""))
            elif message.get("status") in {"failed", "interrupted", "stopped"}:
                lines.append(f"{message.get('id')}: {message.get('status')} — "
                             f"{message.get('errorMessage') or ''}")
        text = "\n".join(lines[-100:])
        if text != self.diagnostics_text.toPlainText():
            self.diagnostics_text.setPlainText(text)

    def _on_disconnected(self) -> None:
        self._set_offline()
        self.connectionLost.emit("聊天连接已断开")

    def _set_offline(self) -> None:
        self._poll_timer.stop()
        self._busy = self._ai_enabled = False
        self._pending_text = None
        self._send_inflight = self._waiting_to_send = False
        self.status_label.setText("离线")
        self._sync_controls()

    def _sync_controls(self) -> None:
        connected = self._client is not None and self._client.is_connected
        self.input_edit.setEnabled(connected and self._ai_enabled)
        self.action_button.setEnabled(connected and self._ai_enabled
                                      and self._pending_text is None)
        self.action_button.setText("发送中…" if self._pending_text else "发送")
        self.stop_button.setVisible(self._busy and connected)
        self.stop_button.setEnabled(not self._waiting_to_send)

    def refresh_theme(self) -> None:
        dark = isDarkTheme()
        bg, fg = ("#23252A", "#F5F5F5") if dark else ("#F3F5F8", "#20242B")
        thumb = "#737882" if dark else "#B6BDC8"
        self.message_area.setStyleSheet(
            f"QScrollArea {{ border: none; background: {bg}; border-radius: 8px; }}"
            f"QScrollBar:vertical {{ background: {bg}; width: 12px; margin: 3px; }}"
            f"QScrollBar::handle:vertical {{ background: {thumb}; min-height: 32px; border-radius: 3px; }}"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }"
            "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }")
        self.message_view.setStyleSheet(f"QWidget#chatMessages {{ background: {bg}; }}")
        self.message_view.setObjectName("chatMessages")
        self.input_edit.setStyleSheet(
            f"QPlainTextEdit {{ color: {fg}; background: {bg}; border: 1px solid {thumb};"
            "border-radius: 8px; padding: 10px; selection-background-color: #2585E6; }")
        self.jump_button.setStyleSheet(
            f"QPushButton {{ background: {'#3A3D44' if dark else '#FFFFFF'}; color: {fg};"
            f"border: 1px solid {thumb}; border-radius: 16px; }}")
        for row in self._rows.values():
            row.refresh_theme()

    @staticmethod
    def _count(value) -> int:
        try:
            return max(0, int(value or 0))
        except (TypeError, ValueError):
            return 0
