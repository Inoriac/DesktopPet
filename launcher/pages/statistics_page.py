"""Read-only runtime, emotion and token dashboard for the selected profile."""

from __future__ import annotations

import math

from PySide6.QtCore import Qt, QTimer, QRectF, QUrl
from PySide6.QtGui import QColor, QDesktopServices, QPainter, QPen
from PySide6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QGridLayout, QTableWidgetItem,
    QHeaderView, QAbstractItemView, QSizePolicy,
)
from qfluentwidgets import (
    BodyLabel, CaptionLabel, SubtitleLabel, TitleLabel, StrongBodyLabel,
    ComboBox, LineEdit, PushButton, TableWidget, ProgressBar, isDarkTheme,
)

from statistics_data import (
    EMOTION_LABELS, ROLE_LABELS, RESULT_LABELS, empty_counters, format_count,
    format_duration, format_time, load_saved_statistics, offline_snapshot,
    profile_directory, recent_result, seven_day_values,
)
from ._ui import ScrollPage, Section


class TokenTrend(QWidget):
    """Seven local calendar days, without a chart dependency."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.values = []
        self.setMinimumHeight(170)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
        self.setToolTip("最近 7 个本地日的已报告总 token；缺少用量的调用不计入柱高。")

    def set_statistics(self, statistics):
        self.values = seven_day_values(statistics)
        self.update()

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing)
        dark = isDarkTheme()
        muted = QColor("#b8c2cd" if dark else "#667381")
        grid = QColor("#434b55" if dark else "#e0e6ec")
        accent = QColor("#68b9ed" if dark else "#237bb5")
        plot = QRectF(6, 30, max(1, self.width()-12), self.height()-62)
        painter.setPen(QPen(grid, 1))
        painter.drawLine(plot.bottomLeft(), plot.bottomRight())
        maximum = max((value for _, value in self.values), default=0)
        if not maximum:
            painter.setPen(muted)
            painter.drawText(plot, Qt.AlignCenter, "近 7 日暂无已报告用量")
        slot = plot.width() / 7
        for index, (label, value) in enumerate(self.values):
            x = plot.left() + slot * index
            height = plot.height() * value / maximum if maximum else 0
            bar = QRectF(x + slot * .25, plot.bottom()-height, slot * .5, height)
            painter.setPen(Qt.NoPen)
            painter.setBrush(accent)
            if value:
                painter.drawRoundedRect(bar, 4, 4)
                painter.setPen(muted)
                number = f"{value/1000:.1f}k" if value >= 1000 else str(value)
                painter.drawText(QRectF(x, bar.top()-24, slot, 22), Qt.AlignCenter, number)
            painter.setPen(muted)
            painter.drawText(QRectF(x, plot.bottom()+8, slot, 22), Qt.AlignCenter, label)
        painter.end()


def _caption(text="", parent=None):
    label = CaptionLabel(text, parent)
    label.setWordWrap(True)
    label.setTextFormat(Qt.PlainText)
    label.setTextColor(QColor("#637080"), QColor("#b9c3ce"))
    return label


def _body(text="", parent=None):
    label = BodyLabel(text, parent)
    label.setWordWrap(True)
    label.setTextFormat(Qt.PlainText)
    label.setMinimumWidth(0)
    return label


def _table(headers):
    table = TableWidget()
    table.setColumnCount(len(headers))
    table.setHorizontalHeaderLabels(headers)
    table.verticalHeader().hide()
    table.setEditTriggers(QAbstractItemView.NoEditTriggers)
    table.setSelectionBehavior(QAbstractItemView.SelectRows)
    table.setSelectionMode(QAbstractItemView.SingleSelection)
    table.setWordWrap(False)
    table.setBorderVisible(True)
    table.setBorderRadius(6)
    table.horizontalHeader().setSectionResizeMode(QHeaderView.Interactive)
    table.horizontalHeader().setStretchLastSection(True)
    table.setMinimumWidth(0)
    table.setSizePolicy(QSizePolicy.Ignored, QSizePolicy.Fixed)
    return table


class StatisticsPage(ScrollPage):
    PAGE_SIZE = 25

    def __init__(self, parent=None):
        super().__init__("StatisticsPage", parent)
        self._profile_id = ""
        self._pet_name = ""
        self._client = None
        self._client_profile_id = ""
        self._client_generation = 0
        self._client_connections = []
        self._snapshot = None
        self._live = False
        self._unsupported = False
        self._warning = ""
        self._page = 0
        self._core_log_path = ""
        self._core_log_profile = ""
        self._poll_timer = QTimer(self)
        self._poll_timer.setInterval(2000)
        self._poll_timer.timeout.connect(self._poll)
        self.vBoxLayout.setContentsMargins(24, 20, 24, 20)
        self._build_header()
        self._build_runtime()
        self._build_usage()
        self._build_history()
        self._build_logs()
        self.refresh_theme()
        self._render()

    def _build_header(self):
        header = QWidget()
        layout = QVBoxLayout(header)
        layout.setContentsMargins(0, 0, 0, 4)
        layout.setSpacing(6)
        title = TitleLabel("运行概览")
        layout.addWidget(title)
        self.status_label = _body()
        layout.addWidget(self.status_label)
        self.updated_label = _caption()
        layout.addWidget(self.updated_label)
        self.warning_label = _body()
        self.warning_label.setObjectName("statisticsWarning")
        layout.addWidget(self.warning_label)
        self.addCard(header)

    def _build_runtime(self):
        card = Section("当前状态", self)
        card.bodyLayout.setSpacing(8)
        row = QWidget()
        grid = QGridLayout(row)
        grid.setContentsMargins(0, 0, 0, 0)
        grid.setHorizontalSpacing(24)
        grid.setColumnStretch(0, 1)
        grid.setColumnStretch(1, 1)
        mood = QWidget()
        mood_layout = QVBoxLayout(mood)
        mood_layout.setContentsMargins(0, 0, 0, 0)
        mood_layout.setSpacing(6)
        self.mood_label = SubtitleLabel("心情不可用")
        self.mood_label.setTextFormat(Qt.PlainText)
        self.mood_label.setWordWrap(True)
        mood_layout.addWidget(self.mood_label)
        self.mood_details = _caption()
        mood_layout.addWidget(self.mood_details)
        self.valence_bar = ProgressBar()
        self.valence_bar.setFixedHeight(5)
        mood_layout.addWidget(self.valence_bar)
        self.mood_scale_label = _caption("消极 ← 心境 → 积极")
        mood_layout.addWidget(self.mood_scale_label)
        grid.addWidget(mood, 0, 0, 3, 1)
        self.runtime_labels = {}
        for index, key in enumerate(("activity", "ai", "autonomy")):
            self.runtime_labels[key] = _body()
            grid.addWidget(self.runtime_labels[key], index, 1)
        card.addWidget(row)
        self.runtime_details = _caption()
        card.addWidget(self.runtime_details)
        self.addCard(card)

    def _build_usage(self):
        card = Section("模型用量", self)
        card.bodyLayout.setSpacing(10)
        toolbar = QWidget()
        layout = QHBoxLayout(toolbar)
        layout.setContentsMargins(0, 0, 0, 0)
        self.range_combo = ComboBox()
        for label, key in (("今天", "today"), ("本次运行", "session"), ("累计", "allTime")):
            self.range_combo.addItem(label, userData=key)
        self.range_combo.setMinimumWidth(130)
        self.range_combo.currentIndexChanged.connect(self._render_usage)
        layout.addWidget(self.range_combo)
        note = _caption("只累计已报告用量 · token")
        note.setAlignment(Qt.AlignRight | Qt.AlignVCenter)
        layout.addWidget(note, 1)
        card.addWidget(toolbar)
        self.metric_labels = {}
        large_metrics = QWidget()
        metrics_grid = QGridLayout(large_metrics)
        metrics_grid.setContentsMargins(0, 0, 0, 0)
        metrics_grid.setHorizontalSpacing(16)
        for index, (key, label) in enumerate((("totalTokens", "总用量"),
                                               ("promptTokens", "输入"),
                                               ("completionTokens", "输出"))):
            metrics_grid.setColumnStretch(index, 1)
            metrics_grid.addWidget(_caption(label), 0, index)
            value = SubtitleLabel("0")
            value.setTextFormat(Qt.PlainText)
            value.setMinimumWidth(0)
            self.metric_labels[key] = value
            metrics_grid.addWidget(value, 1, index)
        card.addWidget(large_metrics)
        outcomes = QWidget()
        outcomes_grid = QGridLayout(outcomes)
        outcomes_grid.setContentsMargins(0, 0, 0, 0)
        outcomes_grid.setHorizontalSpacing(12)
        for index, (key, label) in enumerate((("callCount", "调用"), ("successCount", "成功"),
                                             ("failureCount", "失败"), ("cancelledCount", "取消"),
                                             ("unknownUsageCount", "未报用量"))):
            outcomes_grid.setColumnStretch(index, 1)
            outcomes_grid.addWidget(_caption(label), 0, index)
            value = StrongBodyLabel("0")
            self.metric_labels[key] = value
            outcomes_grid.addWidget(value, 1, index)
        card.addWidget(outcomes)
        self.usage_details = _caption()
        card.addWidget(self.usage_details)
        self.usage_note = _caption(
            "输入包含缓存；缓存和推理是子项，不再计入总量。重试与备用模型调用分别计数；"
            "未报告用量不代表零消耗，中断调用的已报告量可能不完整。")
        card.addWidget(self.usage_note)
        self.addCard(card)

    def _build_history(self):
        card = Section("近 7 日用量", self)
        self.trend_chart = TokenTrend()
        card.addWidget(self.trend_chart)
        self.addCard(card)

        model_card = Section("累计用途与模型", self)
        model_card.addWidget(_caption("模型用途表示对话、日记等工作类型；此表始终展示累计值。"))
        self.model_table = _table(["模型用途", "提供方 / 模型", "调用", "输入", "输出", "总 token"])
        for column, width in enumerate((105, 250, 75, 110, 100, 110)):
            self.model_table.setColumnWidth(column, width)
        self.model_table.setFixedHeight(150)
        model_card.addWidget(self.model_table)
        self.model_empty = _caption("暂无模型明细。旧版导入的累计数据不补造用途或模型。")
        model_card.addWidget(self.model_empty)
        self.addCard(model_card)

        recent_card = Section("近期调用", self)
        recent_card.addWidget(_caption("保留最近 200 次实际模型调用，仅显示元数据。每页 25 条。"))
        filters = QWidget()
        filter_layout = QGridLayout(filters)
        filter_layout.setContentsMargins(0, 0, 0, 0)
        self.role_filter = ComboBox()
        self.role_filter.addItem("所有用途", userData="")
        self.result_filter = ComboBox()
        for label, value in (("所有结果", ""), ("成功", "success"), ("失败", "failure"), ("取消", "cancelled")):
            self.result_filter.addItem(label, userData=value)
        self.model_filter = LineEdit()
        self.model_filter.setPlaceholderText("筛选模型或提供方")
        self.model_filter.setClearButtonEnabled(True)
        for index, widget in enumerate((self.role_filter, self.result_filter, self.model_filter)):
            widget.setMinimumWidth(0)
            filter_layout.addWidget(widget, 0, index)
        filter_layout.setColumnStretch(2, 1)
        self.role_filter.currentIndexChanged.connect(self._filters_changed)
        self.result_filter.currentIndexChanged.connect(self._filters_changed)
        self.model_filter.textChanged.connect(self._filters_changed)
        recent_card.addWidget(filters)
        self.recent_table = _table(["完成时间", "模型用途", "模型", "结果", "输入", "输出", "总 token", "耗时"])
        for column, width in enumerate((155, 105, 170, 70, 90, 90, 95, 85)):
            self.recent_table.setColumnWidth(column, width)
        self.recent_table.setFixedHeight(380)
        recent_card.addWidget(self.recent_table)
        self.recent_empty = _caption("暂无调用记录。")
        recent_card.addWidget(self.recent_empty)
        footer = QWidget()
        footer_layout = QHBoxLayout(footer)
        footer_layout.setContentsMargins(0, 0, 0, 0)
        self.page_label = _caption()
        self.previous_button = PushButton("上一页")
        self.next_button = PushButton("下一页")
        self.previous_button.clicked.connect(lambda: self._change_page(-1))
        self.next_button.clicked.connect(lambda: self._change_page(1))
        footer_layout.addWidget(self.page_label, 1)
        footer_layout.addWidget(self.previous_button)
        footer_layout.addWidget(self.next_button)
        recent_card.addWidget(footer)
        self.addCard(recent_card)

    def _build_logs(self):
        card = Section("诊断日志", self)
        card.addWidget(_caption("详细 AI 日志按角色保存，并按大小轮转。日志可能包含对话正文，分享前请自行检查。"))
        row = QWidget()
        layout = QHBoxLayout(row)
        layout.setContentsMargins(0, 0, 0, 0)
        self.logs_button = PushButton("打开角色日志目录")
        self.core_log_button = PushButton("打开本次核心日志")
        self.logs_button.clicked.connect(self._open_logs)
        self.core_log_button.clicked.connect(self._open_core_log)
        layout.addWidget(self.logs_button)
        layout.addWidget(self.core_log_button)
        layout.addStretch()
        card.addWidget(row)
        self.logs_note = _caption()
        card.addWidget(self.logs_note)
        self.addCard(card)

    def set_profile(self, profile_id: str, pet_name: str):
        if profile_id == self._profile_id:
            self._pet_name = pet_name
            self._render_header()
            return
        self._profile_id, self._pet_name = profile_id, pet_name
        self._snapshot = None
        self._live = False
        self._warning = ""
        self._page = 0
        self.role_filter.setCurrentIndex(0)
        self.result_filter.setCurrentIndex(0)
        self.model_filter.clear()
        self._load_saved()
        self._render()
        self._update_polling()

    def set_client(self, client, profile_id: str):
        for signal, slot in self._client_connections:
            signal.disconnect(slot)
        self._client_connections.clear()
        self._client_generation += 1
        self._client = client
        self._client_profile_id = profile_id
        self._unsupported = False
        self._make_offline()
        if client is not None:
            generation = self._client_generation
            for signal, handler in ((client.overviewReceived, self._receive_overview),
                                    (client.overviewFailed, self._overview_failed),
                                    (client.disconnected, self._disconnected)):
                def slot(*args, handler=handler, generation=generation):
                    if generation == self._client_generation:
                        handler(*args)
                signal.connect(slot)
                self._client_connections.append((signal, slot))
        self._update_polling()

    def set_core_log_path(self, path: str, profile_id: str):
        self._core_log_path, self._core_log_profile = path or "", profile_id
        self._render_logs()

    def _matching_client(self):
        return (self._client is not None and self._client.is_connected
                and self._profile_id == self._client_profile_id and bool(self._profile_id))

    def _load_saved(self):
        if self._profile_id:
            self._snapshot, self._warning = load_saved_statistics(self._profile_id)

    def showEvent(self, event):
        super().showEvent(event)
        if not self._live:
            # Read a newer checkpoint when returning to an offline page.
            saved, warning = load_saved_statistics(self._profile_id) if self._profile_id else (None, "")
            if saved is not None and (self._snapshot is None
                    or str(saved.get("capturedAt", "")) > str(self._snapshot.get("capturedAt", ""))):
                self._snapshot = saved
            if warning:
                self._warning = warning
            if self._snapshot:
                self._snapshot = offline_snapshot(self._snapshot)
            self._render()
        self._update_polling()

    def hideEvent(self, event):
        self._poll_timer.stop()
        super().hideEvent(event)

    def _update_polling(self):
        self._poll_timer.stop()
        if self.isVisible() and self._matching_client() and not self._unsupported:
            self._poll_timer.start()
            self._poll()

    def _poll(self):
        if self.isVisible() and self._matching_client() and not self._unsupported:
            self._client.request_runtime_overview()

    def _receive_overview(self, snapshot):
        if not self._matching_client() or snapshot.get("profileId") != self._profile_id:
            return
        if snapshot.get("schemaVersion") != 1 or not isinstance(snapshot.get("statistics"), dict):
            self._overview_failed("ACTION_UNSUPPORTED", "运行概览版本不兼容")
            return
        self._snapshot = snapshot
        self._live = True
        self._warning = ""
        self._render()

    def _overview_failed(self, code, message):
        if self._profile_id != self._client_profile_id:
            return
        self._unsupported = code == "ACTION_UNSUPPORTED"
        self._warning = "当前核心不支持运行概览，请升级核心；仍可查看已保存统计。" if self._unsupported \
            else f"运行概览暂不可用：{message}"
        if self._unsupported:
            self._poll_timer.stop()
        self._make_offline()

    def _disconnected(self):
        self._poll_timer.stop()
        self._make_offline()

    def _make_offline(self):
        self._live = False
        if self._snapshot:
            self._snapshot = offline_snapshot(self._snapshot)
        self._render()

    def _statistics(self):
        return self._snapshot.get("statistics", {}) if self._snapshot else {}

    def _render(self):
        self._render_header()
        self._render_runtime()
        self._render_usage()
        self.trend_chart.set_statistics(self._statistics())
        self._render_models()
        self._update_role_filter()
        self._render_recent()
        self._render_logs()

    def _render_header(self):
        state = "在线 · 每 2 秒更新" if self._live else "离线 · 非实时统计"
        self.status_label.setText(f"{self._pet_name or '未选择角色'}  ·  {state}")
        updated = self._snapshot.get("capturedAt", "") if self._snapshot else ""
        self.updated_label.setText(f"最近更新：{format_time(updated)}" if updated
                                   else "尚无统计。启动所选角色后，可在这里查看运行状态与模型用量。")
        warning = self._warning or self._statistics().get("persistenceWarning", "")
        self.warning_label.setText(warning)
        self.warning_label.setVisible(bool(warning))

    def _render_runtime(self):
        runtime = self._snapshot.get("runtime") if self._snapshot and self._live else None
        emotion = self._snapshot.get("emotion", {}) if self._snapshot and self._live else {}
        uptime = self._statistics().get("uptime", {})
        if runtime is None:
            for key, label in (("activity", "活动"), ("ai", "AI"), ("autonomy", "自主活动")):
                self.runtime_labels[key].setText(f"{label}：不可用")
            task = "不可用"
        else:
            queue = int(runtime.get("pendingMessageCount", 0))
            activity = "睡眠中" if runtime.get("sleeping") else "处理中" if runtime.get("busy") \
                else "等待处理" if queue else "空闲"
            if queue:
                activity += f" · {queue} 条排队"
            if runtime.get("daydreamRunning"):
                activity += " · 后台联想中"
            self.runtime_labels["activity"].setText(f"活动：{activity}")
            self.runtime_labels["ai"].setText(
                f"AI：{'开启' if runtime.get('aiEnabled') else '关闭'} · "
                f"存储{'就绪' if runtime.get('storageReady') else '未就绪'}")
            autonomy = runtime.get("autonomy", {})
            known_states = {"disabled": "关闭", "paused": "暂停", "failure": "异常",
                            "configuring": "初始化中", "preparing": "准备中", "selecting": "选择活动中",
                            "running": "进行中", "budget_wait": "等待预算恢复", "cooldown": "冷却中",
                            "wait": "暂时等待", "waiting": "等待时机", "ready": "可自主安排"}
            state = "关闭" if not autonomy.get("enabled") else known_states.get(
                autonomy.get("state"), "未就绪" if not autonomy.get("ready") else
                "可自主安排" if autonomy.get("canDeliberate") else "等待时机")
            self.runtime_labels["autonomy"].setText(f"自主活动：{state}")
            self.runtime_labels["autonomy"].setToolTip("\n".join(
                str(autonomy[key]) for key in ("state", "reason", "error") if autonomy.get(key)))
            status = str(runtime.get("taskStatus", "")).lower()
            task = {"received": "已接收", "executing": "执行中", "planning": "规划中", "paused": "已暂停",
                    "completed": "已完成", "declined": "已拒绝", "failed": "失败", "cancelled": "已取消",
                    "assessing": "评估中", "replanning": "重新规划", "observing": "观察结果",
                    "verifying": "核验中", "waitingforuser": "等待用户", "": "无"}.get(status, status)
        self.runtime_details.setText(
            f"任务：{task}  ·  本次在线 {format_duration(uptime.get('sessionMs') if self._live else None)}"
            f"  ·  累计在线 {format_duration(uptime.get('totalMs', 0))}"
            f"  ·  触摸 {format_count(uptime.get('touchCount', 0))} 次")
        if emotion.get("available"):
            self.mood_label.setText(EMOTION_LABELS.get(emotion.get("active"), str(emotion.get("active", ""))))
            valence = float(emotion.get("moodValence", 0))
            self.mood_details.setText(
                f"心境 {valence:+.2f} · 活跃度 {float(emotion.get('moodArousal', 0)):.0%}"
                f" · 强度 {float(emotion.get('intensity', 0)):.0%}")
            self.valence_bar.setValue(round((valence + 1) * 50))
            self.valence_bar.show()
            self.mood_scale_label.show()
            self.mood_details.setToolTip(
                f"可信度 {float(emotion.get('confidence', 0)):.0%} · 情绪更新 {format_time(emotion.get('updatedAt', ''))}")
        else:
            self.mood_label.setText("情绪已关闭" if self._live and emotion.get("enabled") is False else "心情不可用")
            self.mood_details.setText("开启情绪系统后显示。" if self._live and emotion.get("enabled") is False
                                      else "等待当前角色的实时状态。")
            self.mood_details.setToolTip("")
            self.valence_bar.hide()
            self.mood_scale_label.hide()

    def _render_usage(self, *_args):
        if not hasattr(self, "metric_labels"):
            return
        key = self.range_combo.currentData()
        counters = self._statistics().get(key, empty_counters())
        unavailable = key == "session" and not self._live
        for name, label in self.metric_labels.items():
            label.setText(format_count(None if unavailable else (counters or {}).get(name, 0)))
        if unavailable:
            self.usage_details.setText("本次运行用量仅在当前角色在线时可用。累计与今天仍可离线查看。")
        else:
            counters = counters or {}
            self.usage_details.setText(
                f"缓存 token {format_count(counters.get('cachedTokens', 0))} · "
                f"缓存命中 {format_count(counters.get('promptCacheHitTokens', 0))} · "
                f"未命中 {format_count(counters.get('promptCacheMissTokens', 0))} · "
                f"推理 {format_count(counters.get('reasoningTokens', 0))}")
            if key == "allTime" and self._statistics().get("legacyImported"):
                self.usage_details.setText(self.usage_details.text() + "\n含旧版导入累计；对应日期和模型明细不可追溯。")

    @staticmethod
    def _fill_table(table, rows):
        scroll = table.verticalScrollBar().value()
        horizontal = table.horizontalScrollBar().value()
        selected = table.currentRow()
        table.setUpdatesEnabled(False)
        table.setRowCount(len(rows))
        for row_index, cells in enumerate(rows):
            for column, value in enumerate(cells):
                item = QTableWidgetItem(str(value))
                item.setToolTip(str(value))
                table.setItem(row_index, column, item)
        if 0 <= selected < len(rows):
            table.selectRow(selected)
        table.verticalScrollBar().setValue(scroll)
        table.horizontalScrollBar().setValue(horizontal)
        table.setUpdatesEnabled(True)

    def _render_models(self):
        models = self._statistics().get("byModel", [])
        self._fill_table(self.model_table, [
            (ROLE_LABELS.get(row.get("role"), row.get("role", "未分类")),
             f"{row.get('provider', '')} / {row.get('model', '')}",
             *(format_count(row.get(key, 0)) for key in ("callCount", "promptTokens", "completionTokens", "totalTokens")))
            for row in models
        ])
        self.model_empty.setVisible(not models)
        self.model_table.setVisible(bool(models))
        self.model_table.setFixedHeight(min(250, 45 + len(models)*36))

    def _update_role_filter(self):
        roles = sorted({str(row.get("role", "unclassified")) for row in self._statistics().get("recentCalls", [])})
        previous = self.role_filter.currentData()
        if previous and previous not in roles:
            roles.append(previous)
        current = [self.role_filter.itemData(i) for i in range(1, self.role_filter.count())]
        if current == roles:
            return
        self.role_filter.blockSignals(True)
        self.role_filter.clear()
        self.role_filter.addItem("所有用途", userData="")
        for role in roles:
            self.role_filter.addItem(ROLE_LABELS.get(role, role), userData=role)
        self.role_filter.setCurrentIndex(max(0, self.role_filter.findData(previous)))
        self.role_filter.blockSignals(False)

    def _filtered_calls(self):
        role, result, text = self.role_filter.currentData(), self.result_filter.currentData(), self.model_filter.text().strip().lower()
        return [row for row in self._statistics().get("recentCalls", [])
                if (not role or row.get("role") == role)
                and (not result or recent_result(row) == result)
                and (not text or text in f"{row.get('model', '')} {row.get('provider', '')}".lower())]

    def _render_recent(self):
        calls = self._filtered_calls()
        pages = max(1, math.ceil(len(calls)/self.PAGE_SIZE))
        self._page = min(self._page, pages-1)
        rows = []
        page_calls = calls[self._page*self.PAGE_SIZE:(self._page+1)*self.PAGE_SIZE]
        for call in page_calls:
            usage = call.get("usage", {})
            rows.append((format_time(call.get("completedAt", ""), short=True),
                         ROLE_LABELS.get(call.get("role"), call.get("role", "未分类")),
                         call.get("model", ""), RESULT_LABELS[recent_result(call)],
                         *(format_count(usage.get(key, 0)) if call.get("usageReported") else "未报告"
                           for key in ("promptTokens", "completionTokens", "totalTokens")),
                         f"{call.get('durationMs', 0)/1000:.2f}s"))
        self._fill_table(self.recent_table, rows)
        for row_index, call in enumerate(page_calls):
            tooltip = f"提供方：{call.get('provider', '')}\n路由：{call.get('routeId', '')}\n调用 ID：{call.get('attemptId', '')}"
            self.recent_table.item(row_index, 2).setToolTip(tooltip)
        self.page_label.setText(f"{self._page+1} / {pages} 页 · {len(calls)} 条")
        self.previous_button.setEnabled(self._page > 0)
        self.next_button.setEnabled(self._page+1 < pages)
        self.recent_empty.setText("没有符合筛选条件的调用。" if self._statistics().get("recentCalls") else "暂无调用记录。")
        self.recent_empty.setVisible(not calls)
        self.recent_table.setVisible(bool(calls))

    def _filters_changed(self, *_args):
        self._page = 0
        self._render_recent()

    def _change_page(self, offset):
        self._page = max(0, self._page + offset)
        self._render_recent()

    def _render_logs(self):
        try:
            directory = profile_directory(self._profile_id) / "logs"
        except ValueError:
            directory = None
        self.logs_button.setEnabled(directory is not None)
        self.logs_note.setText(str(directory) if directory else "选择角色后可查看日志位置。")
        self.core_log_button.setEnabled(bool(self._core_log_path) and self._core_log_profile == self._profile_id)

    def _open_logs(self):
        directory = profile_directory(self._profile_id) / "logs"
        if not directory.is_dir():
            self.logs_note.setText("该角色尚未生成详细 AI 日志。")
            return
        if not QDesktopServices.openUrl(QUrl.fromLocalFile(str(directory))):
            self.logs_note.setText(f"无法打开日志目录：{directory}")

    def _open_core_log(self):
        if self._core_log_profile == self._profile_id and self._core_log_path:
            if not QDesktopServices.openUrl(QUrl.fromLocalFile(self._core_log_path)):
                self.logs_note.setText("无法打开本次核心日志。")

    def refresh_theme(self):
        dark = isDarkTheme()
        self.warning_label.setStyleSheet("color: #f1c875;" if dark else "color: #8e5b05;")
        self.trend_chart.update()
