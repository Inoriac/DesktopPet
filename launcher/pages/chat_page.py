"""Private conversation, paged history and asynchronous chat commands."""

from __future__ import annotations

from PySide6.QtCore import QEvent, Qt, QTimer, Signal
from PySide6.QtGui import QKeyEvent
from PySide6.QtWidgets import (
    QAbstractSlider, QFrame, QGridLayout, QHBoxLayout, QLabel, QLayout, QPlainTextEdit,
    QPushButton, QScrollArea, QVBoxLayout, QWidget,
)
from qfluentwidgets import (
    CaptionLabel, InfoBar, InfoBarPosition,
    StrongBodyLabel, PushButton, PrimaryPushButton, isDarkTheme,
)

from .chat_widgets import MessageRow, message_datetime


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
        self._recent_messages: list[dict] = []
        self._diagnostic_messages: list[dict] = []
        self._recent_has_more_before = False
        self._has_more_before = False
        self._has_more_after = False
        self._profile_id = None
        self._history_serial = 0
        self._history_request_token = ""
        self._history_error = False
        self._follow_bottom = True
        self._restoring_scroll = False
        self._render_generation = 0
        self._scroll_anchor = None
        self._user_scroll_serial = 0
        self._known_statuses: dict[str, str] | None = None
        self._pending_text: str | None = None
        self._send_inflight = False
        self._last_error = ""
        self._diagnostic_events: list[str] = []
        self._task: dict = {}
        self._task_action_pending = False
        self._task_feedback = ""

        root = QVBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)
        self.header = QFrame(self)
        self.header.setObjectName("chatHeader")
        header = QHBoxLayout(self.header)
        header.setContentsMargins(22, 16, 22, 16)
        header.setSpacing(12)
        self.header_avatar = QLabel("聊", self.header)
        self.header_avatar.setFixedSize(40, 40)
        self.header_avatar.setAlignment(Qt.AlignCenter)
        header.addWidget(self.header_avatar)
        identity = QVBoxLayout()
        identity.setSpacing(3)
        self.title_label = StrongBodyLabel("聊天", self.header)
        self.title_label.setTextFormat(Qt.PlainText)
        self.title_label.setStyleSheet("font-size: 17px;")
        self.status_label = CaptionLabel("离线", self.header)
        identity.addWidget(self.title_label)
        identity.addWidget(self.status_label)
        header.addLayout(identity)
        header.addStretch(1)
        root.addWidget(self.header)

        self.task_card = QFrame(self)
        task_layout = QVBoxLayout(self.task_card)
        task_layout.setContentsMargins(12, 10, 12, 10)
        self.task_title = StrongBodyLabel("当前任务", self.task_card)
        self.task_title.setTextFormat(Qt.PlainText)
        self.task_title.setWordWrap(True)
        self.task_detail = QLabel(self.task_card)
        self.task_detail.setTextFormat(Qt.PlainText)
        self.task_detail.setWordWrap(True)
        self.task_detail.setTextInteractionFlags(Qt.TextSelectableByMouse)
        task_layout.addWidget(self.task_title)
        task_layout.addWidget(self.task_detail)
        self.task_controls = QGridLayout()
        self.task_controls.setHorizontalSpacing(8)
        self.task_controls.setVerticalSpacing(6)
        self.task_continue = PushButton("继续任务", self.task_card)
        self.task_continue.setToolTip("可先在下方输入补充说明，再继续任务")
        self.task_continue.clicked.connect(lambda: self._task_action("resume_task"))
        self.task_extend = PushButton("增加额度并继续", self.task_card)
        self.task_extend.setToolTip("增加 12 次工具使用、2 次方案调整和 5 分钟处理时间")
        self.task_extend.clicked.connect(lambda: self._task_action("resume_task", extend_budget=True))
        self.task_pause = PushButton("暂停", self.task_card)
        self.task_pause.clicked.connect(lambda: self._task_action("pause_task"))
        self.task_stop = PushButton("停止任务", self.task_card)
        self.task_stop.clicked.connect(lambda: self._task_action("stop_task"))
        self.task_confirm_done = PushButton("确认动作已完成", self.task_card)
        self.task_confirm_done.clicked.connect(lambda: self._task_action("resolve_task_call", succeeded=True))
        self.task_confirm_absent = PushButton("确认动作未发生", self.task_card)
        self.task_confirm_absent.clicked.connect(lambda: self._task_action("resolve_task_call", succeeded=False))
        self._task_buttons = (self.task_continue, self.task_extend, self.task_pause,
                              self.task_confirm_done, self.task_confirm_absent, self.task_stop)
        task_layout.addLayout(self.task_controls)
        self.task_card.hide()
        root.addWidget(self.task_card)

        self.message_area = QScrollArea(self)
        self.message_area.setWidgetResizable(True)
        self.message_area.setFrameShape(QFrame.NoFrame)
        self.message_area.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        self.message_area.setVerticalScrollBarPolicy(Qt.ScrollBarAlwaysOn)
        self.message_view = QWidget(self.message_area)
        self.message_layout = QVBoxLayout(self.message_view)
        self.message_layout.setContentsMargins(22, 14, 22, 22)
        self.message_layout.setSpacing(18)
        self.message_layout.setSizeConstraint(QLayout.SetMinAndMaxSize)
        self.history_button = QPushButton("向上滚动查看更早消息", self.message_view)
        self.history_button.setFixedHeight(26)
        self.history_button.setFlat(True)
        self.history_button.clicked.connect(self._load_older)
        self.message_layout.addWidget(self.history_button, 0, Qt.AlignHCenter)
        self.empty_label = QLabel("和桌宠聊点什么吧", self.message_view)
        self.empty_label.setAlignment(Qt.AlignCenter)
        self.message_layout.addWidget(self.empty_label)
        self.message_layout.addStretch(1)
        self.message_area.setWidget(self.message_view)
        root.addWidget(self.message_area, 1)
        self.jump_button = QPushButton("↓ 回到最新", self.message_area.viewport())
        self.jump_button.setObjectName("chatJumpToBottom")
        self.jump_button.setToolTip("查看最新消息")
        self.jump_button.setFixedSize(112, 34)
        self.jump_button.clicked.connect(self.scroll_to_bottom)
        self.jump_button.hide()
        self.message_area.viewport().installEventFilter(self)
        self.message_area.installEventFilter(self)
        scrollbar = self.message_area.verticalScrollBar()
        scrollbar.installEventFilter(self)
        scrollbar.actionTriggered.connect(self._scroll_action)
        scrollbar.sliderMoved.connect(lambda value: self._user_scrolled(value < scrollbar.value()))
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
        root.addWidget(self.diagnostics_panel)

        self.composer = QFrame(self)
        self.composer.setObjectName("chatComposer")
        composer = QVBoxLayout(self.composer)
        composer.setContentsMargins(22, 10, 22, 16)
        composer.setSpacing(6)
        self.composer_toolbar = QHBoxLayout()
        self.composer_toolbar.setSpacing(10)
        self.composer_label = CaptionLabel("消息", self.composer)
        self.composer_toolbar.addWidget(self.composer_label)
        self.composer_toolbar.addStretch(1)
        self.debug_button = PushButton("调试信息", self.composer)
        self.debug_button.setCheckable(True)
        self.debug_button.setToolTip("查看调用统计与技术诊断")
        self.debug_button.setFixedHeight(28)
        self.debug_button.toggled.connect(self.diagnostics_panel.setVisible)
        self.composer_toolbar.addWidget(self.debug_button)
        composer.addLayout(self.composer_toolbar)
        self.input_edit = ChatInput(self.composer)
        self.input_edit.setPlaceholderText("发送消息…")
        self.input_edit.setFixedHeight(78)
        self.input_edit.submitRequested.connect(self._submit)
        composer.addWidget(self.input_edit)
        controls = QHBoxLayout()
        self.input_hint = CaptionLabel("Enter 发送 · Shift + Enter 换行", self.composer)
        controls.addWidget(self.input_hint)
        controls.addStretch(1)
        self.stop_button = PushButton("停止回复", self.composer)
        self.stop_button.clicked.connect(self._stop)
        controls.addWidget(self.stop_button)
        self.action_button = PrimaryPushButton("发送", self.composer)
        self.action_button.setMinimumWidth(88)
        self.action_button.setFixedHeight(32)
        self.action_button.setToolTip("发送消息")
        self.action_button.clicked.connect(self._submit)
        controls.addWidget(self.action_button)
        composer.addLayout(controls)
        root.addWidget(self.composer)

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
        self._send_inflight = False
        self._busy = self._ai_enabled = False
        self._task = {}
        self._task_action_pending = False
        self._task_feedback = ""
        self.task_card.hide()
        self._profile_id = None
        self._last_error = ""
        if client is not None:
            # A new profile must not show the previous profile's conversation.
            self._reset_history()
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
                (client.operationFailed, self._operation_failed),
                (client.historyFailed, self._history_failed))

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
        profile_id = str(state.get("profileId") or "")
        if self._profile_id is not None and profile_id != self._profile_id:
            self._reset_history()
            self._known_statuses = None
            self._last_error = ""
            self._diagnostic_events.clear()
        self._profile_id = profile_id
        name = str(state.get("petName") or "桌宠")
        self.title_label.setText(name)
        self.header_avatar.setText(name[:1].upper())
        self._busy = bool(state.get("busy"))
        self._ai_enabled = bool(state.get("aiEnabled"))
        self._task = state.get("task") if isinstance(state.get("task"), dict) else {}
        pending_count = int(state.get("pendingMessageCount") or 0)
        self.status_label.setText(
            f"已接收，等待处理 {pending_count} 条消息" if pending_count else
            "正在回复…" if self._busy else ("● 在线" if self._ai_enabled else "AI 未启用"))
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
        diagnostics = state.get("diagnosticMessages") or []
        self._diagnostic_messages = [m for m in diagnostics if isinstance(m, dict)]
        status_messages = messages + self._diagnostic_messages
        previous = self._known_statuses
        self._known_statuses = {str(m.get("id")): str(m.get("status"))
                                for m in status_messages}
        recent = [m for m in messages if self._visible_message(m)]
        self._recent_messages = recent
        self._recent_has_more_before = bool(state.get("hasMoreBefore"))
        # A pending empty reply leaves the visible snapshot on failure. Remove
        # its old thinking bubble before merging the new recent window.
        hidden_ids = {str(m.get("id")) for m in status_messages if not self._visible_message(m)}
        self._messages = [m for m in self._messages if str(m.get("id")) not in hidden_ids]
        if not self._messages:
            self._messages = list(recent)
            self._has_more_before = self._recent_has_more_before
            self._has_more_after = False
        elif recent:
            old_ids = {str(m.get("id")) for m in self._messages}
            if old_ids.intersection(str(m.get("id")) for m in recent) and not self._has_more_after:
                self._messages = self._merge_messages(self._messages, recent)
            elif self._follow_bottom:
                self._messages = list(recent)
                self._has_more_before = self._recent_has_more_before
                self._has_more_after = False
            else:
                # A disconnected interval or a located history window can
                # have an unseen gap. Keep it separate until "回到最新".
                self._has_more_after = True
        self._render_messages(self._messages)
        self._update_diagnostics()
        if previous is not None:
            failures = [m for m in status_messages
                        if m.get("status") in {"failed", "interrupted"}
                        and previous.get(str(m.get("id"))) != m.get("status")]
            if failures:
                failed = failures[-1]
                self._show_problem("回复未完成", self._friendly_error(
                    str(failed.get("errorMessage") or "")),
                                   str(failed.get("errorMessage") or ""),
                                   str(failed.get("id") or ""))
        self._sync_controls()

    @staticmethod
    def _visible_message(message: dict) -> bool:
        if message.get("role") not in {"user", "assistant"}:
            return False
        # Diagnostics belong to the debug panel, not an empty error bubble.
        return bool(message.get("content")) or message.get("status") in {
            "pending", "streaming"}

    def _reset_history(self) -> None:
        self._history_serial += 1
        self._user_scroll_serial += 1
        self._history_request_token = ""
        self._history_error = False
        self._messages = []
        self._recent_messages = []
        self._diagnostic_messages = []
        self._has_more_before = self._recent_has_more_before = False
        self._has_more_after = False
        self._follow_bottom = True
        self._scroll_anchor = None

    @staticmethod
    def _merge_messages(earlier: list[dict], later: list[dict]) -> list[dict]:
        updates = {str(m.get("id")): m for m in later}
        ids = {str(m.get("id")) for m in earlier}
        return [updates.get(str(m.get("id")), m) for m in earlier] + [
            m for m in later if str(m.get("id")) not in ids]

    def _capture_anchor(self):
        position = self.message_area.verticalScrollBar().value()
        for mid, row in sorted(self._rows.items(), key=lambda pair: pair[1].y()):
            if row.y() + row.height() >= position:
                return mid, row.y() - position
        return None

    def _row_width(self) -> int:
        return max(100, self.message_area.viewport().width() - 44)

    def _render_messages(self, messages: list[dict]) -> None:
        scrollbar = self.message_area.verticalScrollBar()
        position = scrollbar.value()
        following = self._follow_bottom
        anchor = None if following else self._capture_anchor()
        self._scroll_anchor = anchor
        self._restoring_scroll = True
        selected = [m for m in messages if self._visible_message(m)]
        ids = {str(m.get("id")) for m in selected}
        for mid in list(self._rows):
            if mid not in ids:
                row = self._rows.pop(mid)
                self.message_layout.removeWidget(row)
                row.hide()
                row.deleteLater()
        previous_time = None
        for index, message in enumerate(selected, 2):
            mid = str(message.get("id"))
            row = self._rows.get(mid)
            if row is None:
                row = MessageRow(message, self.message_view)
                self._rows[mid] = row
            else:
                row.update_message(message)
            if self.message_layout.indexOf(row) != index:
                self.message_layout.insertWidget(index, row)
            row.set_sender_name(self.title_label.text())
            timestamp = message_datetime(message)
            show_time = timestamp.isValid() and (previous_time is None or not previous_time.isValid()
                         or previous_time.secsTo(timestamp) >= 300
                         or previous_time.date() != timestamp.date())
            row.set_time_label(timestamp.toString("MM月dd日 HH:mm") if show_time else "")
            row.set_available_width(self._row_width())
            previous_time = timestamp
        self.empty_label.setVisible(not selected)
        self._update_history_button()
        self._render_generation += 1
        generation = self._render_generation

        def restore():
            if generation != self._render_generation:
                return
            self.message_layout.activate()
            if following:
                scrollbar.setValue(scrollbar.maximum())
            elif anchor and anchor[0] in self._rows:
                scrollbar.setValue(self._rows[anchor[0]].y() - anchor[1])
            else:
                scrollbar.setValue(position)
            def settled():
                if generation != self._render_generation:
                    return
                if following:
                    scrollbar.setValue(scrollbar.maximum())
                elif anchor and anchor[0] in self._rows:
                    scrollbar.setValue(self._rows[anchor[0]].y() - anchor[1])
                self._restoring_scroll = False
                self._scroll_anchor = None
                self._update_jump_button()
            QTimer.singleShot(0, self, settled)
        QTimer.singleShot(0, self, restore)

    def _update_history_button(self) -> None:
        loading = bool(self._history_request_token)
        text = ("正在加载…" if loading else "加载失败，点击重试" if self._history_error
                else "向上滚动查看更早消息" if self._has_more_before else "已显示最早的消息")
        self.history_button.setText(text)
        self.history_button.setVisible(bool(self._messages))
        self.history_button.setEnabled(self._has_more_before and not loading
                                       and self._client is not None and self._client.is_connected)

    def _load_older(self) -> None:
        if self._history_request_token or not self._has_more_before or not self._messages \
                or self._client is None or not self._client.is_connected:
            return
        self._history_serial += 1
        self._history_request_token = f"older:{self._history_serial}"
        self._history_error = False
        self._follow_bottom = False
        self._update_history_button()
        self._client.request_history(before_message_id=str(self._messages[0]["id"]),
                                     limit=self.PAGE_SIZE, request_token=self._history_request_token)

    def _history_loaded(self, result: dict) -> None:
        if not self._history_request_token or result.get("requestToken") != self._history_request_token:
            return
        self._history_request_token = ""
        self._history_error = False
        earlier = [m for m in result.get("messages", []) if isinstance(m, dict) and self._visible_message(m)]
        self._messages = self._merge_messages(earlier, self._messages)
        self._has_more_before = bool(result.get("hasMoreBefore"))
        self._render_messages(self._messages)

    def _history_failed(self, action: str, token: str, detail: str) -> None:
        if action != "get_chat_history" or not self._history_request_token or token != self._history_request_token:
            return
        self._history_request_token = ""
        self._history_error = True
        self._diagnostic_events.append(f"历史消息: {detail}")
        self._diagnostic_events = self._diagnostic_events[-30:]
        self._update_diagnostics()
        self._update_history_button()

    def scroll_to_bottom(self) -> None:
        self._history_request_token = ""
        self._history_error = False
        self._has_more_after = False
        self._has_more_before = self._recent_has_more_before
        self._messages = list(self._recent_messages)
        self._follow_bottom = True
        self._render_messages(self._messages)

    def _scroll_changed(self, value: int) -> None:
        if not self._restoring_scroll:
            self._follow_bottom = not self._has_more_after and (
                self.message_area.verticalScrollBar().maximum() - value <= 24)
        self._update_jump_button()

    def _scroll_action(self, action: int) -> None:
        bar = self.message_area.verticalScrollBar()
        upward = action in (QAbstractSlider.SliderSingleStepSub.value, QAbstractSlider.SliderPageStepSub.value,
                            QAbstractSlider.SliderToMinimum.value)
        if action == QAbstractSlider.SliderMove.value:
            if bar.sliderPosition() == bar.value():
                return
            upward = bar.sliderPosition() < bar.value()
        self._user_scrolled(upward)

    def _user_scrolled(self, allow_older: bool = True) -> None:
        # Only user events arm pagination. Range/layout changes never request
        # another page, including the range change caused by a prepend.
        self._user_scroll_serial += 1
        serial = self._user_scroll_serial
        if self._restoring_scroll:
            self._render_generation += 1
            self._restoring_scroll = False
            self._scroll_anchor = None
        self._follow_bottom = False
        def after_scroll():
            if serial != self._user_scroll_serial:
                return
            self._scroll_changed(self.message_area.verticalScrollBar().value())
            if allow_older and self.message_area.verticalScrollBar().value() <= 60:
                self._load_older()
        QTimer.singleShot(0, self, after_scroll)

    def _scroll_range_changed(self, _minimum: int, maximum: int) -> None:
        if self._follow_bottom:
            self.message_area.verticalScrollBar().setValue(maximum)
        elif self._restoring_scroll and self._scroll_anchor:
            mid, offset = self._scroll_anchor
            if mid in self._rows:
                self.message_area.verticalScrollBar().setValue(self._rows[mid].y() - offset)
        self._update_jump_button()

    def _update_jump_button(self) -> None:
        bar = self.message_area.verticalScrollBar()
        self.jump_button.setVisible(self._has_more_after or bar.maximum() - bar.value() > 48)
        viewport = self.message_area.viewport()
        self.jump_button.move(max(0, viewport.width() - 134), max(0, viewport.height() - 50))
        self.jump_button.raise_()

    def eventFilter(self, watched, event) -> bool:
        viewport = self.message_area.viewport()
        if watched is viewport and event.type() == QEvent.Resize:
            if self._rows:
                self._render_messages(self._messages)
            self._update_jump_button()
        if event.type() == QEvent.Wheel:
            delta = event.pixelDelta().y() or event.angleDelta().y()
            if delta:
                self._user_scrolled(delta > 0)
        elif event.type() == QEvent.KeyPress and event.key() in (
                Qt.Key_Up, Qt.Key_PageUp, Qt.Key_Home, Qt.Key_Down, Qt.Key_PageDown, Qt.Key_End):
            self._user_scrolled(event.key() in (Qt.Key_Up, Qt.Key_PageUp, Qt.Key_Home))
            if event.key() in (Qt.Key_Home, Qt.Key_End):
                self.message_area.verticalScrollBar().triggerAction(
                    QAbstractSlider.SliderToMinimum if event.key() == Qt.Key_Home
                    else QAbstractSlider.SliderToMaximum)
                return True
        return super().eventFilter(watched, event)

    def resizeEvent(self, event) -> None:
        super().resizeEvent(event)
        self.input_hint.setVisible(self.width() >= 520)

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
            if not self._send_inflight:
                self._pending_text = None
            self._client.stop_response()
            self._sync_controls()

    def _unknown_task_call(self) -> dict:
        for step in self._task.get("steps") or []:
            if isinstance(step, dict) and step.get("status") == "unknown" \
                    and not step.get("read_only", True):
                return step
        return {}

    def _task_action(self, action: str, extend_budget: bool = False,
                     succeeded: bool = False) -> None:
        if self._client is None or not self._client.is_connected \
                or self._task_action_pending or not self._task.get("id"):
            return
        self._task_action_pending = True
        self._sync_controls()
        task_id = str(self._task["id"])
        if action == "resume_task":
            self._task_feedback = self.input_edit.toPlainText().strip()
            self._client.resume_task(task_id, self._task_feedback, extend_budget)
        elif action == "resolve_task_call":
            call = self._unknown_task_call()
            if call:
                self._client.resolve_task_call(task_id, str(call["id"]), succeeded)
            else:
                self._task_action_pending = False
                self._sync_controls()
        elif action == "pause_task":
            self._client.pause_task(task_id)
        elif action == "stop_task":
            self._client.stop_task(task_id)

    def _operation_finished(self, operation: str, _result: dict) -> None:
        if operation == "get_chat_history":
            self._history_loaded(_result)
            return
        self._last_error = ""
        if operation in {"resume_task", "pause_task", "stop_task", "resolve_task_call"}:
            self._task_action_pending = False
            if operation == "resume_task" and self._task_feedback \
                    and self.input_edit.toPlainText().strip() == self._task_feedback:
                self.input_edit.clear()
            self._task_feedback = ""
        if operation == "send_message":
            if self.input_edit.toPlainText().strip() == self._pending_text:
                self.input_edit.clear()
            self._pending_text = None
            self._send_inflight = False
            self.scroll_to_bottom()
        self._sync_controls()
        self.poll_once()

    def _operation_failed(self, operation: str, detail: str) -> None:
        if operation in {"resume_task", "pause_task", "stop_task", "resolve_task_call"}:
            self._task_action_pending = False
            self._task_feedback = ""
            self._show_problem("任务暂时无法继续", detail or "请查看任务进度后重试。")
            self._sync_controls()
            self.poll_once()
            return
        if operation in {"send_message", "stop_response"}:
            self._pending_text = None
            self._send_inflight = False
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
        for message in self._diagnostic_messages + self._messages:
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
        self._user_scroll_serial += 1
        self._busy = self._ai_enabled = False
        self._pending_text = None
        self._send_inflight = False
        self._task_action_pending = False
        self._history_request_token = ""
        self._update_history_button()
        self.status_label.setText("离线")
        self._sync_controls()

    def _sync_controls(self) -> None:
        connected = self._client is not None and self._client.is_connected
        status_color = "#1687F8" if self._busy else "#459969" if connected and self._ai_enabled else "#8993A3"
        self.status_label.setStyleSheet(f"color: {status_color};")
        self.input_edit.setEnabled(connected and self._ai_enabled)
        self.action_button.setEnabled(connected and self._ai_enabled
                                      and self._pending_text is None)
        self.action_button.setText("发送中…" if self._pending_text else "发送")
        self.stop_button.setVisible(self._busy and connected)
        self.stop_button.setEnabled(connected)
        self._sync_task_card(connected)

    def _sync_task_card(self, connected: bool) -> None:
        task = self._task
        self.task_card.setVisible(bool(task.get("id")))
        if not task.get("id"):
            return
        status = str(task.get("status") or "")
        names = {"Received": "已收到", "Assessing": "正在考虑任务", "Planning": "正在安排步骤",
                 "Executing": "正在处理", "Observing": "正在整理资料", "Verifying": "正在核实结果",
                 "Replanning": "正在调整方案", "WaitingForUser": "等待你的回应", "Paused": "已暂停",
                 "Completed": "已完成", "Declined": "暂不受理", "Failed": "未能完成", "Cancelled": "已停止"}
        self.task_title.setText(f"{names.get(status, '任务')} · {task.get('goal') or ''}")
        terminal = status in {"Completed", "Declined", "Failed", "Cancelled"}
        waiting = status in {"WaitingForUser", "Paused"}
        unknown = self._unknown_task_call()
        detail = str(task.get("reason") or "") if waiting or terminal else str(task.get("plan") or "正在准备…")
        if terminal and task.get("result"):
            result = str(task["result"])
            detail = result if status == "Completed" or not detail else detail + "\n\n" + result
        if waiting and not unknown:
            detail += "\n可在下方补充说明，然后点击继续任务。"
        if unknown:
            detail += "\n请核对上一次提醒或播放等动作的实际结果，再确认是否发生。"
        self.task_detail.setText(detail[:1600])
        self.task_continue.setVisible(waiting and not unknown)
        budget = task.get("budget") or {}
        exhausted = (self._count(task.get("tool_calls")) >= self._count(budget.get("max_tool_calls", 12))
                     or self._count(task.get("replans")) >= self._count(budget.get("max_replans", 2))
                     or self._count(task.get("active_ms")) >= self._count(budget.get("max_active_ms", 300000)))
        self.task_extend.setVisible(waiting and exhausted and not unknown)
        self.task_pause.setVisible(not waiting and not terminal)
        self.task_stop.setVisible(not terminal)
        self.task_confirm_done.setVisible(waiting and bool(unknown))
        self.task_confirm_absent.setVisible(waiting and bool(unknown))
        for button in self._task_buttons:
            self.task_controls.removeWidget(button)
            button.setEnabled(connected and self._ai_enabled and not self._task_action_pending)
        for index, button in enumerate(button for button in self._task_buttons if not button.isHidden()):
            self.task_controls.addWidget(button, index // 2, index % 2, Qt.AlignLeft)

    def refresh_theme(self) -> None:
        dark = isDarkTheme()
        bg, fg = ("#22262E", "#F0F2F5") if dark else ("#F3F5F8", "#262B35")
        surface = "#292D35" if dark else "#FFFFFF"
        line = "#383D47" if dark else "#E8ECF1"
        muted = "#9EA6B3" if dark else "#8993A3"
        thumb = "#56606E" if dark else "#C7CFDA"
        self.header.setStyleSheet(f"QFrame#chatHeader {{ background: {surface}; border-bottom: 1px solid {line}; }}")
        self.header_avatar.setStyleSheet(
            f"background: {'#474364' if dark else '#E4DFFA'}; color: {'#D5CEFF' if dark else '#796AB4'};"
            "border-radius: 13px; font-size: 20px; font-weight: 600;")
        self.message_area.setStyleSheet(
            f"QScrollArea {{ border: none; background: {bg}; }}"
            f"QScrollBar:vertical {{ background: {bg}; width: 10px; margin: 3px; }}"
            f"QScrollBar::handle:vertical {{ background: {thumb}; min-height: 32px; border-radius: 3px; }}"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }"
            "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }")
        self.message_view.setObjectName("chatMessages")
        self.message_view.setStyleSheet(f"QWidget#chatMessages {{ background: {bg}; }}")
        self.history_button.setStyleSheet(
            f"QPushButton {{ color: {muted}; border: none; background: transparent; font-size: 12px; }}"
            "QPushButton:hover:enabled { color: #1687F8; }")
        self.empty_label.setStyleSheet(f"color: {muted}; padding: 32px; background: transparent;")
        self.composer.setStyleSheet(f"QFrame#chatComposer {{ background: {surface}; border-top: 1px solid {line}; }}")
        self.composer_label.setStyleSheet(f"color: {muted};")
        self.input_hint.setStyleSheet(f"color: {muted};")
        self.action_button.setStyleSheet(
            "PrimaryPushButton { background: #1687F8; color: white; border: none; border-radius: 6px; }"
            "PrimaryPushButton:hover { background: #3598FA; }"
            "PrimaryPushButton:pressed { background: #0879EA; }"
            f"PrimaryPushButton:disabled {{ background: {line}; color: {muted}; }}")
        self.input_edit.setStyleSheet(
            f"QPlainTextEdit {{ color: {fg}; background: {surface}; border: none;"
            "padding: 4px 0px; font-size: 14px; selection-background-color: #1687F8; }")
        self.task_card.setStyleSheet(
            f"QFrame {{ background: {surface}; border: none; }}"
            f"QLabel {{ color: {fg}; background: transparent; }}")
        self.jump_button.setStyleSheet(
            f"QPushButton {{ background: {surface}; color: #1687F8;"
            f"border: 1px solid {line}; border-radius: 16px; }}")
        for row in self._rows.values():
            row.refresh_theme()
        self.title_label.setStyleSheet("font-size: 17px;")
        self._sync_controls()

    @staticmethod
    def _count(value) -> int:
        try:
            return max(0, int(value or 0))
        except (TypeError, ValueError):
            return 0
