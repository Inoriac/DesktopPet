"""调试与测试触发页面：手动触发自主活动、外部会话处理、主动搭话、屏幕观察与记忆整理。"""

from __future__ import annotations

import json
import sqlite3
from datetime import datetime

from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QColor
from PySide6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QPlainTextEdit,
)
from qfluentwidgets import (
    BodyLabel, CaptionLabel, ComboBox, FluentIcon as FIF, InfoBar,
    InfoBarPosition, PrimaryPushButton, PushButton, SwitchButton, TitleLabel,
    isDarkTheme,
)

from statistics_data import profile_directory
from ._ui import ScrollPage, Section


class DebugPage(ScrollPage):
    def __init__(self, parent=None):
        super().__init__("DebugPage", parent)
        self._profile_id = ""
        self._pet_name = ""
        self._client = None
        self._client_profile_id = ""
        self._client_generation = 0
        self._client_connections = []
        self._poll_timer = QTimer(self)
        self._poll_timer.setInterval(1500)
        self._poll_timer.timeout.connect(self.refresh_diagnostics)
        self.vBoxLayout.setContentsMargins(24, 20, 24, 20)

        self._build_header()
        self._build_autonomy_triggers()
        self._build_proactive_triggers()
        self._build_inspector()
        self.addStretch()
        self.refresh_theme()
        self._sync_controls()

    def _build_header(self) -> None:
        header = QWidget()
        layout = QVBoxLayout(header)
        layout.setContentsMargins(0, 0, 0, 4)
        layout.setSpacing(6)
        layout.addWidget(TitleLabel("调试与测试触发"))
        self.status_label = BodyLabel("当前状态：未连接桌宠进程")
        layout.addWidget(self.status_label)
        hint = CaptionLabel(
            "用于直接触发平时需要等待空闲/定时器/预算窗口才能运行的后台功能（如 QQ 自主回复、主动搭话、屏幕观察、记忆整理），并查看实时队列。")
        hint.setWordWrap(True)
        hint.setTextColor(QColor("#637080"), QColor("#b9c3ce"))
        layout.addWidget(hint)
        self.addCard(header)

    def _build_autonomy_triggers(self) -> None:
        card = Section("自主活动与外部会话 (QQ / 插件)", self)

        self.bypass_switch = SwitchButton()
        self.bypass_switch.setChecked(True)
        card.addRow(
            "触发前重置冷却与每小时预算",
            "开启后每次手动触发都会清空 60s 冷却、退避与每小时额度限制，方便连续测试",
            self.bypass_switch,
        )

        conv_wrap = QWidget()
        conv_layout = QHBoxLayout(conv_wrap)
        conv_layout.setContentsMargins(0, 0, 0, 0)
        conv_layout.setSpacing(8)
        self.conv_combo = ComboBox()
        self.conv_combo.setMinimumWidth(300)
        self.conv_combo.addItem("自动选择（优先待处理外部 QQ 线索，否则内部探索）", userData="")
        self.refresh_conv_btn = PushButton(FIF.SYNC, "刷新会话")
        self.refresh_conv_btn.clicked.connect(self.refresh_diagnostics)
        conv_layout.addWidget(self.conv_combo, 1)
        conv_layout.addWidget(self.refresh_conv_btn)
        card.addRow(
            "目标会话 (Context)",
            "选择要立即触发自主评估的会话范围",
            conv_wrap,
        )

        action_wrap = QWidget()
        action_layout = QHBoxLayout(action_wrap)
        action_layout.setContentsMargins(0, 0, 0, 0)
        action_layout.setSpacing(10)
        self.trigger_autonomy_btn = PrimaryPushButton(FIF.PLAY, "立即触发自主活动评估")
        self.trigger_autonomy_btn.clicked.connect(self._trigger_autonomy)
        self.reset_budget_btn = PushButton(FIF.BROOM, "仅重置预算与冷却")
        self.reset_budget_btn.clicked.connect(
            lambda: self._trigger("reset_autonomy_budget"))
        action_layout.addWidget(self.trigger_autonomy_btn)
        action_layout.addWidget(self.reset_budget_btn)
        card.addRow(
            "手动触发自主决策",
            "立即向 ActivityRuntime 发送机会信号，调用 agencyDecision 模型评估并执行",
            action_wrap,
        )
        self.addCard(card)

    def _build_proactive_triggers(self) -> None:
        card = Section("主动行为与后台维护触发", self)

        proactive_btn = PushButton(FIF.CHAT, "立即触发主动搭话")
        proactive_btn.clicked.connect(lambda: self._trigger("proactive_chat"))
        card.addRow(
            "主动搭话 (proactive_chat)",
            "不等待 3–5 分钟定时器，立即让桌宠结合最近上下文主动说一句话",
            proactive_btn,
        )

        idle_btn = PushButton(FIF.PLAY, "立即触发待机动作")
        idle_btn.clicked.connect(lambda: self._trigger("idle_action"))
        card.addRow(
            "空闲动作思考 (idle_action)",
            "立即触发一次 idle_action 模型思考与动作工具调用",
            idle_btn,
        )

        screen_btn = PushButton(FIF.PHOTO, "立即触发屏幕识别对话")
        screen_btn.clicked.connect(lambda: self._trigger("screen_chat"))
        card.addRow(
            "屏幕观察 (screen_chat)",
            "立即截取当前屏幕并调用视觉模型（vision）识别后搭话",
            screen_btn,
        )

        daydream_btn = PushButton(FIF.SYNC, "立即触发记忆整理 (Daydream)")
        daydream_btn.clicked.connect(lambda: self._trigger("daydream"))
        card.addRow(
            "后台联想与记忆巩固 (Daydream)",
            "不等待 5 分钟空闲，立即整理短期收件箱记忆并建图",
            daydream_btn,
        )

        fragment_btn = PushButton(FIF.EDIT, "立即收集日记便签")
        fragment_btn.clicked.connect(lambda: self._trigger("diary_fragment"))
        card.addRow(
            "日记便签收集 (diary_fragment)",
            "不等待 5 分钟空闲，立即将近期新事件写成一段加密日记草稿便签",
            fragment_btn,
        )

        sleep_btn = PushButton(FIF.DOCUMENT, "立即触发睡眠写日记")
        sleep_btn.clicked.connect(lambda: self._trigger("sleep_cycle"))
        card.addRow(
            "睡眠整理与私人日记 (sleep_cycle)",
            "不等待 30 分钟空闲，立即触发人格巩固与当日加密私人日记撰写",
            sleep_btn,
        )

        self._trigger_buttons = (
            self.trigger_autonomy_btn,
            self.reset_budget_btn,
            proactive_btn,
            idle_btn,
            screen_btn,
            daydream_btn,
            fragment_btn,
            sleep_btn,
        )
        self.addCard(card)

    def _build_inspector(self) -> None:
        card = Section("实时调度状态与外部线索队列", self)
        toolbar = QWidget()
        toolbar_layout = QHBoxLayout(toolbar)
        toolbar_layout.setContentsMargins(0, 0, 0, 0)
        self.inspector_updated_label = CaptionLabel("尚未加载")
        self.inspector_updated_label.setTextColor(QColor("#637080"), QColor("#b9c3ce"))
        refresh_btn = PushButton(FIF.SYNC, "立即刷新状态")
        refresh_btn.clicked.connect(self.refresh_diagnostics)
        toolbar_layout.addWidget(self.inspector_updated_label, 1)
        toolbar_layout.addWidget(refresh_btn)
        card.addWidget(toolbar)

        self.inspector_text = QPlainTextEdit()
        self.inspector_text.setReadOnly(True)
        self.inspector_text.setMinimumHeight(280)
        card.addWidget(self.inspector_text)
        self.addCard(card)

    def set_profile(self, profile_id: str, pet_name: str) -> None:
        self._profile_id = profile_id
        self._pet_name = pet_name
        self.refresh_diagnostics()

    def set_client(self, client, profile_id: str) -> None:
        for signal, slot in self._client_connections:
            try:
                signal.disconnect(slot)
            except (RuntimeError, TypeError):
                pass
        self._client_connections.clear()
        self._client_generation += 1
        self._client = client
        self._client_profile_id = profile_id
        if client is not None:
            generation = self._client_generation
            for signal, handler in (
                (client.operationFinished, self._on_operation_finished),
                (client.operationFailed, self._on_operation_failed),
                (client.disconnected, self._on_disconnected),
            ):
                def slot(*args, handler=handler, generation=generation):
                    if generation == self._client_generation:
                        handler(*args)
                signal.connect(slot)
                self._client_connections.append((signal, slot))
        self._sync_controls()
        self.refresh_diagnostics()
        if self.isVisible() and self._is_online():
            self._poll_timer.start()
        else:
            self._poll_timer.stop()

    def showEvent(self, event) -> None:
        super().showEvent(event)
        self.refresh_diagnostics()
        if self._is_online():
            self._poll_timer.start()

    def hideEvent(self, event) -> None:
        self._poll_timer.stop()
        super().hideEvent(event)

    def _is_online(self) -> bool:
        return (self._client is not None and self._client.is_connected
                and bool(self._profile_id)
                and self._profile_id == self._client_profile_id)

    def _sync_controls(self) -> None:
        online = self._is_online()
        self.status_label.setText(
            f"当前角色：{self._pet_name or '未选择'}  ·  "
            f"{'● 在线（可直接触发测试）' if online else '离线（请先启动桌宠）'}"
        )
        for btn in getattr(self, "_trigger_buttons", ()):
            btn.setEnabled(online)

    def _trigger_autonomy(self) -> None:
        context_id = str(self.conv_combo.currentData() or "")
        bypass = self.bypass_switch.isChecked()
        self._trigger(
            "autonomy_opportunity",
            contextId=context_id,
            bypassCooldown=bypass,
        )

    def _trigger(self, target: str, **kwargs) -> None:
        if not self._is_online():
            InfoBar.warning(
                "桌宠未运行", "请先在「宠物」页启动桌宠后再触发测试",
                parent=self.window(), position=InfoBarPosition.TOP, duration=3000)
            return
        self._client.debug_trigger(target, **kwargs)

    def _on_operation_finished(self, action: str, data: dict) -> None:
        if action != "debug_trigger":
            return
        msg = str(data.get("message") or "调试指令已执行")
        InfoBar.success(
            "触发已发送", msg, parent=self.window(),
            position=InfoBarPosition.TOP, duration=4000)
        QTimer.singleShot(300, self.refresh_diagnostics)

    def _on_operation_failed(self, action: str, detail: str) -> None:
        if action != "debug_trigger":
            return
        InfoBar.error(
            "触发失败", detail or "调试触发未能完成",
            parent=self.window(), position=InfoBarPosition.TOP, duration=5000)
        self.refresh_diagnostics()

    def _on_disconnected(self) -> None:
        self._poll_timer.stop()
        self._sync_controls()
        self.refresh_diagnostics()

    def refresh_diagnostics(self) -> None:
        self._sync_controls()
        if not self._profile_id:
            self.inspector_text.setPlainText("请先选择角色。")
            return
        try:
            rt_db = profile_directory(self._profile_id) / "agent_runtime.sqlite"
        except ValueError as error:
            self.inspector_text.setPlainText(str(error))
            return
        if not rt_db.is_file():
            self.inspector_text.setPlainText(f"尚未创建运行时数据库：{rt_db}")
            return

        lines = []
        conversations = []
        try:
            conn = sqlite3.connect(str(rt_db), timeout=1.0)
            try:
                for row in conn.execute(
                    "SELECT id, channel, connection_key, remote_id, kind, revision "
                    "FROM agent_conversations ORDER BY channel, remote_id"
                ):
                    conversations.append(row)

                state_row = conn.execute(
                    "SELECT revision, control_json FROM agent_runtime_state WHERE profile_id=?",
                    (self._profile_id,),
                ).fetchone()
                if state_row:
                    rev, raw_ctrl = state_row
                    ctrl = json.loads(raw_ctrl)
                    budget = ctrl.get("hourlyBudget") or {}
                    attempts = budget.get("attempts") or {}
                    cfg = ctrl.get("autonomyConfig") or {}
                    now_logical = int((ctrl.get("opportunityClock") or {}).get("logical") or 0)
                    active_attempts = [
                        a for a in attempts.values()
                        if isinstance(a, dict) and (
                            int(a.get("at") or 0) > now_logical - 3600000
                            or not a.get("settled")
                        )
                    ]
                    sel_count = sum(1 for a in active_attempts if a.get("purpose") == "selection")
                    used_tokens = sum(int(a.get("tokens") or 0) for a in active_attempts)
                    used_cost = sum(int(a.get("cost") or 0) for a in active_attempts)
                    lines.append(
                        f"=== 自主调度状态 (revision={rev}) ===\n"
                        f"autonomyEnabled : {ctrl.get('autonomyEnabled')}    paused: {ctrl.get('paused')}\n"
                        f"lastDecision    : {json.dumps(ctrl.get('lastDecision') or {}, ensure_ascii=False)}\n"
                        f"lastDecisionAt  : {ctrl.get('lastDecisionAt', '无')}\n"
                        f"nextConsiderAt  : {ctrl.get('nextConsiderAt', '立即可考虑')} (backoff={ctrl.get('backoff', 0)} ms)\n"
                        f"近1小时预算占用 : 选择调用 {sel_count}/{cfg.get('maxSelectionCallsPerHour', 12)} 次 · "
                        f"Token {used_tokens:,}/{cfg.get('maxTokensPerHour', 300000):,} · "
                        f"估算成本 {used_cost:,}/{cfg.get('maxEstimatedCostMicrosPerHour', 1000000):,}"
                    )

                cues = conn.execute(
                    "SELECT migration_id, state, cursor_json FROM agent_runtime_migrations "
                    "WHERE profile_id=? AND migration_id LIKE 'observation:cue:%' "
                    "ORDER BY rowid DESC LIMIT 10",
                    (self._profile_id,),
                ).fetchall()
                cue_lines = [f"=== 外部观察线索队列 (observation:cue, 最近 {len(cues)} 条) ==="]
                for mid, st, raw_c in cues:
                    c_obj = json.loads(raw_c)
                    cue_lines.append(
                        f"  [{st}] conv={c_obj.get('conversationId')} "
                        f"msg={c_obj.get('messageId')} event={c_obj.get('eventId')}"
                    )
                if not cues:
                    cue_lines.append("  (暂无外部线索)")
                lines.append("\n".join(cue_lines))

                acts = conn.execute(
                    "SELECT id, state, focus_owned, intent_json, execution_json, updated_at "
                    "FROM agent_activities WHERE profile_id=? ORDER BY rowid DESC LIMIT 6",
                    (self._profile_id,),
                ).fetchall()
                act_lines = [f"=== 近期自主/复杂活动 (agent_activities, 最近 {len(acts)} 条) ==="]
                for aid, st, focus, raw_intent, raw_exec, upd in acts:
                    intent = json.loads(raw_intent)
                    exec_obj = json.loads(raw_exec) if raw_exec else {}
                    call_status = dict(
                        conn.execute(
                            "SELECT call_id, status FROM agent_action_calls "
                            "WHERE profile_id=? AND activity_id=?",
                            (self._profile_id, aid),
                        ).fetchall()
                    )
                    steps = exec_obj.get("steps") or []
                    step_names = [
                        f"{s.get('name')}({s.get('status') or call_status.get(s.get('id'), 'pending')})"
                        for s in steps[-5:]
                    ]
                    detail_line = f"    steps={len(steps)} {step_names}" if steps else "    steps=0"
                    if exec_obj.get("reason"):
                        detail_line += f"  reason={str(exec_obj.get('reason'))[:120]}"
                    if exec_obj.get("result"):
                        detail_line += f"\n    result={str(exec_obj.get('result')).replace(chr(10), ' ')[:180]}"
                    act_lines.append(
                        f"  [{st}{'/前台' if focus else ''}] {upd} · ID={aid[:8]}…\n"
                        f"    origin={intent.get('origin')}  goal={intent.get('goal')}\n"
                        f"    caps={json.dumps(intent.get('capabilityRefs') or [], ensure_ascii=False)}\n"
                        f"{detail_line}"
                    )
                if not acts:
                    act_lines.append("  (暂无活动记录)")
                lines.append("\n".join(act_lines))

                ext_msgs = conn.execute(
                    "SELECT conversation_id, direction, occurred_at, payload_json "
                    "FROM agent_messages WHERE profile_id=? AND conversation_id != 'local_owner' "
                    "ORDER BY rowid DESC LIMIT 8",
                    (self._profile_id,),
                ).fetchall()
                msg_lines = [f"=== 外部会话消息归档 (最近 {len(ext_msgs)} 条) ==="]
                for cid, direction, occ, raw_p in ext_msgs:
                    p = json.loads(raw_p)
                    spk = (p.get("externalSpeaker") or {}).get("remoteActorId") or p.get("senderKind", "")
                    msg_lines.append(
                        f"  {occ} [{direction}] conv={cid[:8]}… speaker={spk}: {p.get('text', '')[:100]}"
                    )
                if not ext_msgs:
                    msg_lines.append("  (暂无外部会话消息)")
                lines.append("\n".join(msg_lines))

                diary_cnt = conn.execute(
                    "SELECT COUNT(*) FROM event_log WHERE profile_id=? AND type='DiaryStored'",
                    (self._profile_id,),
                ).fetchone()[0]
                thought_cnt = conn.execute(
                    "SELECT COUNT(*) FROM event_log WHERE profile_id=? AND type='InnerThoughtStored'",
                    (self._profile_id,),
                ).fetchone()[0]
                has_sleep_tbl = conn.execute(
                    "SELECT 1 FROM sqlite_master WHERE type='table' AND name='sleep_sessions'"
                ).fetchone()
                sleep_rows = (
                    conn.execute(
                        "SELECT session_id, state, decision, updated_at FROM sleep_sessions "
                        "WHERE profile_id=? ORDER BY rowid DESC LIMIT 3",
                        (self._profile_id,),
                    ).fetchall()
                    if has_sleep_tbl
                    else []
                )
                diary_lines = [
                    f"=== 私密反思与私人日记状态 ===",
                    f"  已归档加密日记事件 (DiaryStored): {diary_cnt} 条  ·  内心独白 (InnerThoughtStored): {thought_cnt} 条",
                ]
                for sid, sst, sdec, supd in sleep_rows:
                    diary_lines.append(
                        f"  睡眠会话 {sid[:8]}… · state={sst} · decision={sdec} · updated={supd}"
                    )
                if not sleep_rows:
                    diary_lines.append("  (暂无睡眠写日记会话记录)")
                lines.append("\n".join(diary_lines))
            finally:
                conn.close()
        except (sqlite3.Error, OSError, json.JSONDecodeError) as error:
            lines.append(f"读取运行时数据库失败：{error}")

        # Update conversation selector items while preserving current selection
        current_data = self.conv_combo.currentData()
        ext_convs = [c for c in conversations if c[0] != "local_owner"]
        expected_items = [("", "自动选择（优先待处理外部 QQ 线索，否则内部探索）")]
        for cid, channel, conn_key, remote_id, kind, _rev in ext_convs:
            expected_items.append((cid, f"[{channel}/{kind}] {remote_id} ({cid[:8]}…)"))
        current_items = [
            (self.conv_combo.itemData(i), self.conv_combo.itemText(i))
            for i in range(self.conv_combo.count())
        ]
        if current_items != expected_items:
            self.conv_combo.blockSignals(True)
            self.conv_combo.clear()
            for data_val, label_text in expected_items:
                self.conv_combo.addItem(label_text, userData=data_val)
            idx = self.conv_combo.findData(current_data)
            self.conv_combo.setCurrentIndex(max(0, idx))
            self.conv_combo.blockSignals(False)

        self.inspector_updated_label.setText(
            f"最近刷新：{datetime.now().strftime('%H:%M:%S')}")
        text = "\n\n".join(lines)
        if text != self.inspector_text.toPlainText():
            scroll = self.inspector_text.verticalScrollBar().value()
            self.inspector_text.setPlainText(text)
            self.inspector_text.verticalScrollBar().setValue(scroll)

    def refresh_theme(self) -> None:
        dark = isDarkTheme()
        surface = "#292D35" if dark else "#FFFFFF"
        fg = "#F0F2F5" if dark else "#262B35"
        line = "#383D47" if dark else "#E8ECF1"
        self.inspector_text.setStyleSheet(
            f"QPlainTextEdit {{ background: {surface}; color: {fg}; "
            f"border: 1px solid {line}; border-radius: 6px; padding: 10px; "
            f"font-family: Consolas, 'Microsoft YaHei UI', monospace; font-size: 12px; }}"
        )
