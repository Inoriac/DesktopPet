"""记忆库可视化浏览页面：查看近期活跃记忆池、海马体工作集、高重要度记忆与全量记忆库。"""

from __future__ import annotations

import json
import math
import sqlite3

from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QColor
from PySide6.QtWidgets import (
    QAbstractItemView, QDialog, QGridLayout, QHBoxLayout, QHeaderView,
    QPlainTextEdit, QSizePolicy, QTableWidgetItem, QVBoxLayout, QWidget,
)
from qfluentwidgets import (
    BodyLabel, CaptionLabel, ComboBox, FluentIcon as FIF, LineEdit,
    PushButton, StrongBodyLabel, SubtitleLabel, TableWidget, TitleLabel,
    isDarkTheme,
)

from statistics_data import format_time, profile_directory
from ._ui import ScrollPage, Section


PARTITION_LABELS = {
    "hippocampus": "海马体 (Hippocampus)",
    "episodic": "情景皮质 (Episodic)",
    "semantic": "语义皮质 (Semantic)",
    "preference": "偏好皮质 (Preference)",
    "procedural": "程序皮质 (Procedural)",
}
TYPE_LABELS = {
    "working": "工作记忆 (working)",
    "short_term": "短期记忆 (short_term)",
    "episodic": "情景经历 (episodic)",
    "semantic": "语义事实 (semantic)",
    "preference": "偏好习惯 (preference)",
    "procedural": "操作技能 (procedural)",
    "task_shadow": "任务投影 (task_shadow)",
    "relationship": "人物关系 (relationship)",
    "core": "核心身份 (core)",
    "event": "事件记录 (event)",
}
STATUS_LABELS = {
    "active": "活跃 (active)",
    "consolidated": "已巩固 (consolidated)",
    "archived": "已归档 (archived)",
    "superseded": "已替代 (superseded)",
    "expired": "已过期 (expired)",
    "cancelled": "已取消 (cancelled)",
    "deleted": "已删除 (deleted)",
}


def _caption(text="", parent=None):
    label = CaptionLabel(text, parent)
    label.setWordWrap(True)
    label.setTextFormat(Qt.PlainText)
    label.setTextColor(QColor("#637080"), QColor("#b9c3ce"))
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


class MemoryDetailDialog(QDialog):
    """Display complete fields, tags, evidence, relations, and attribution for a memory."""

    def __init__(self, detail: dict, parent=None):
        super().__init__(parent)
        self.setAttribute(Qt.WA_DeleteOnClose)
        item = detail.get("item") or {}
        self.setWindowTitle(
            f"记忆详情 · {item.get('partition', '')} / {item.get('type', '')} · {item.get('id', '')[:8]}…"
        )
        self.resize(860, 680)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(20, 16, 20, 16)
        layout.setSpacing(10)

        header_text = (
            f"ID：{item.get('id', '')}    "
            f"分区：{PARTITION_LABELS.get(item.get('partition'), item.get('partition', ''))}    "
            f"类型：{TYPE_LABELS.get(item.get('type'), item.get('type', ''))}    "
            f"状态：{STATUS_LABELS.get(item.get('status'), item.get('status', ''))}\n"
            f"重要度 (importance)：{float(item.get('importance') or 0):.2f}    "
            f"强度 (strength)：{float(item.get('strength') or 0):.2f}    "
            f"置信度 (confidence)：{float(item.get('confidence') or 0):.2f}    "
            f"提及 / 访问：{int(item.get('mention_count') or 0)} / {int(item.get('access_count') or 0)}"
        )
        header = BodyLabel(header_text, self)
        header.setWordWrap(True)
        layout.addWidget(header)

        self.viewer = QPlainTextEdit(self)
        self.viewer.setReadOnly(True)
        self.viewer.setPlainText(self._format_detail(detail))
        layout.addWidget(self.viewer, 1)

        footer = QHBoxLayout()
        footer.addStretch(1)
        close_btn = PushButton("关闭", self)
        close_btn.clicked.connect(self.close)
        footer.addWidget(close_btn)
        layout.addLayout(footer)
        self._apply_theme()

    def _apply_theme(self):
        dark = isDarkTheme()
        bg, fg = ("#22262E", "#F0F2F5") if dark else ("#F3F5F8", "#262B35")
        surface = "#292D35" if dark else "#FFFFFF"
        line = "#383D47" if dark else "#E8ECF1"
        self.setStyleSheet(f"QDialog {{ background: {bg}; color: {fg}; }}")
        self.viewer.setStyleSheet(
            f"QPlainTextEdit {{ background: {surface}; color: {fg}; "
            f"border: 1px solid {line}; border-radius: 6px; padding: 10px; "
            f"font-family: Consolas, 'Microsoft YaHei UI', monospace; font-size: 13px; }}"
        )

    @staticmethod
    def _format_detail(detail: dict) -> str:
        item = detail.get("item") or {}
        tags = detail.get("tags") or []
        activation = detail.get("activation")
        attribution = detail.get("attribution")
        people = detail.get("people") or []
        origins = detail.get("origins") or []
        evidence = detail.get("evidence") or []
        relations = detail.get("relations") or []
        access_logs = detail.get("access_logs") or []

        sections = [
            "=== 记忆内容 ===",
            f"Key            : {item.get('key', '')}",
            f"Summary (摘要) : {item.get('summary', '')}",
            f"Content (正文) :\n{item.get('content', '')}",
            "",
            "=== 属性与时间戳 ===",
            f"Privacy Level  : {item.get('privacy_level', '')}",
            f"Scope / Source : {item.get('scope', '')} / {item.get('source', '')}",
            f"Emotion        : {item.get('emotion', '')} (intensity={item.get('emotion_intensity', 0)}, confidence={item.get('emotion_confidence', 0)})",
            f"Created At     : {format_time(str(item.get('created_at') or ''))}",
            f"Updated At     : {format_time(str(item.get('updated_at') or ''))}",
            f"Last Mentioned : {format_time(str(item.get('last_mentioned_at') or ''))}",
            f"Last Accessed  : {format_time(str(item.get('last_accessed_at') or ''))}",
            f"Expires At     : {format_time(str(item.get('expires_at') or '')) if item.get('expires_at') else '不过期'}",
            f"Tags (标签)    : {', '.join(tags) if tags else '(无)'}",
        ]

        if activation:
            sections.extend([
                "",
                "=== 近期活跃池快照 (ActiveMemoryPool) ===",
                f"Activation     : {float(activation.get('activation') or 0):.4f}",
                f"Source         : {activation.get('source', '')}",
                f"Context ID     : {activation.get('context_id', '')}",
                f"Last Activated : {format_time(str(activation.get('last_activated_at') or ''))}",
            ])

        if attribution:
            sections.extend([
                "",
                "=== 归属与可见性 (Attribution) ===",
                f"Claim Kind     : {attribution.get('claim_kind', '')} (resolution={attribution.get('resolution', '')})",
                f"Audience       : {attribution.get('audience_json', '')}",
                f"People         : {json.dumps(people, ensure_ascii=False) if people else '[]'}",
                f"Origins        : {json.dumps(origins, ensure_ascii=False, indent=2) if origins else '[]'}",
            ])

        if relations:
            rel_lines = ["", f"=== 图谱关联边 (Relations: 共 {len(relations)} 条) ==="]
            for r in relations:
                rel_lines.append(
                    f"  {r.get('from_memory_id', '')[:8]}… --[{r.get('relation_type')}]--> "
                    f"{r.get('to_memory_id', '')[:8]}…  "
                    f"(weight={r.get('weight')}, conf={r.get('confidence')}, prov={r.get('provenance')})"
                )
            sections.extend(rel_lines)

        if evidence:
            ev_lines = ["", f"=== 原始证据 (Evidence: 共 {len(evidence)} 条) ==="]
            for ev in evidence:
                ev_lines.append(
                    f"  [{ev.get('created_at', '')}] ({ev.get('source', '')}): {ev.get('raw_text', '')}"
                )
            sections.extend(ev_lines)

        if access_logs:
            acc_lines = ["", f"=== 近期召回日志 (Access Log: 最近 {len(access_logs)} 条) ==="]
            for acc in access_logs:
                acc_lines.append(
                    f"  [{acc.get('created_at', '')}] score={acc.get('score')} reason={acc.get('reason', '')}"
                )
            sections.extend(acc_lines)

        payload_raw = str(item.get("payload_json") or "").strip()
        if payload_raw and payload_raw != "{}":
            try:
                payload_pretty = json.dumps(json.loads(payload_raw), ensure_ascii=False, indent=2)
            except json.JSONDecodeError:
                payload_pretty = payload_raw
            sections.extend(["", "=== 扩展 Payload JSON ===", payload_pretty])

        return "\n".join(sections)


class MemoryPage(ScrollPage):
    PAGE_SIZE = 30

    def __init__(self, parent=None):
        super().__init__("MemoryPage", parent)
        self._profile_id = ""
        self._pet_name = ""
        self._page = 0
        self._current_rows: list[dict] = []
        self._poll_timer = QTimer(self)
        self._poll_timer.setInterval(4000)
        self._poll_timer.timeout.connect(self.refresh_data)
        self.vBoxLayout.setContentsMargins(24, 20, 24, 20)

        self._build_header()
        self._build_summary_cards()
        self._build_browser()
        self.addStretch()

    def _build_header(self) -> None:
        header = QWidget()
        layout = QVBoxLayout(header)
        layout.setContentsMargins(0, 0, 0, 4)
        layout.setSpacing(6)
        top = QHBoxLayout()
        top.addWidget(TitleLabel("记忆库"))
        top.addStretch(1)
        self.refresh_btn = PushButton(FIF.SYNC, "刷新数据库")
        self.refresh_btn.clicked.connect(self.refresh_data)
        top.addWidget(self.refresh_btn)
        layout.addLayout(top)
        self.status_label = BodyLabel("未选择角色")
        layout.addWidget(self.status_label)
        layout.addWidget(_caption(
            "直接读取当前角色的 SQLite 记忆库（memory.db）。可切换查看「近期活跃记忆池」「海马体工作集」「高重要度排行」与「全记忆池」，点击任意行查看完整记忆详情与图谱关系。"
        ))
        self.addCard(header)

    def _build_summary_cards(self) -> None:
        card = Section("各池与皮质分区概览", self)
        grid_wrap = QWidget()
        grid = QGridLayout(grid_wrap)
        grid.setContentsMargins(0, 0, 0, 0)
        grid.setHorizontalSpacing(16)
        grid.setVerticalSpacing(8)
        self.counter_labels: dict[str, SubtitleLabel] = {}
        items = (
            ("active_pool", "近期活跃池 (ActivePool)"),
            ("hippocampus_active", "海马体待巩固 (活跃)"),
            ("long_term_active", "皮质长期记忆 (活跃)"),
            ("high_importance", "高重要度 (≥0.70)"),
            ("relations", "记忆图谱关系边"),
            ("total", "全库总条目 (含归档/过期)"),
        )
        for idx, (key, label_text) in enumerate(items):
            col = idx % 3
            row = (idx // 3) * 2
            grid.setColumnStretch(col, 1)
            grid.addWidget(_caption(label_text), row, col)
            val = SubtitleLabel("0")
            val.setTextFormat(Qt.PlainText)
            self.counter_labels[key] = val
            grid.addWidget(val, row + 1, col)
        card.addWidget(grid_wrap)
        self.partition_breakdown_label = _caption()
        card.addWidget(self.partition_breakdown_label)
        self.addCard(card)

    def _build_browser(self) -> None:
        card = Section("记忆池浏览与检索", self)
        card.bodyLayout.setSpacing(10)

        filters = QWidget()
        fl = QGridLayout(filters)
        fl.setContentsMargins(0, 0, 0, 0)
        fl.setHorizontalSpacing(10)
        fl.setVerticalSpacing(8)

        self.pool_combo = ComboBox()
        for label, key in (
            ("全记忆池 (All Memories)", "all"),
            ("近期活跃记忆池 (ActiveMemoryPool)", "active_pool"),
            ("海马体工作集 (Hippocampus 待巩固)", "hippocampus"),
            ("高重要度记忆 (按 Importance 排序)", "important"),
            ("皮质长期记忆 (Semantic / Preference / Episodic)", "cortical"),
        ):
            self.pool_combo.addItem(label, userData=key)

        self.partition_combo = ComboBox()
        self.partition_combo.addItem("所有分区", userData="")
        for key, label in PARTITION_LABELS.items():
            self.partition_combo.addItem(label, userData=key)

        self.status_combo = ComboBox()
        for label, key in (
            ("仅活跃与已巩固 (active / consolidated)", "active_only"),
            ("所有状态 (含已归档/已过期)", ""),
            ("仅活跃 (active)", "active"),
            ("仅已归档 (archived)", "archived"),
            ("仅已过期 (expired)", "expired"),
            ("仅已替代 (superseded)", "superseded"),
        ):
            self.status_combo.addItem(label, userData=key)

        self.sort_combo = ComboBox()
        for label, key in (
            ("按重要度排序 (Importance ↓)", "importance"),
            ("按更新时间排序 (Updated ↓)", "updated"),
            ("按强度排序 (Strength ↓)", "strength"),
            ("按访问/提及频次排序 (Access ↓)", "access"),
            ("按活跃度排序 (Activation ↓)", "activation"),
        ):
            self.sort_combo.addItem(label, userData=key)

        self.search_edit = LineEdit()
        self.search_edit.setPlaceholderText("搜索摘要、正文、Key 或标签…")
        self.search_edit.setClearButtonEnabled(True)

        fl.addWidget(self.pool_combo, 0, 0, 1, 2)
        fl.addWidget(self.partition_combo, 0, 2)
        fl.addWidget(self.status_combo, 0, 3)
        fl.addWidget(self.sort_combo, 1, 0)
        fl.addWidget(self.search_edit, 1, 1, 1, 3)
        for col in range(4):
            fl.setColumnStretch(col, 1)

        self.pool_combo.currentIndexChanged.connect(self._on_pool_preset_changed)
        self.partition_combo.currentIndexChanged.connect(self._on_filter_changed)
        self.status_combo.currentIndexChanged.connect(self._on_filter_changed)
        self.sort_combo.currentIndexChanged.connect(self._on_filter_changed)
        self.search_edit.textChanged.connect(self._on_filter_changed)
        card.addWidget(filters)

        self.table = _table([
            "分区 / 类型", "状态", "重要度", "强度 / 激活", "访问/提及", "标签", "摘要 / 内容", "更新时间",
        ])
        for col, width in enumerate((135, 75, 68, 95, 78, 130, 260, 135)):
            self.table.setColumnWidth(col, width)
        self.table.setFixedHeight(440)
        self.table.cellClicked.connect(self._on_row_clicked)
        card.addWidget(self.table)

        self.empty_label = _caption("没有符合当前筛选条件的记忆条目。")
        card.addWidget(self.empty_label)

        footer = QWidget()
        footer_layout = QHBoxLayout(footer)
        footer_layout.setContentsMargins(0, 0, 0, 0)
        self.page_label = _caption()
        self.prev_btn = PushButton("上一页")
        self.next_btn = PushButton("下一页")
        self.prev_btn.clicked.connect(lambda: self._change_page(-1))
        self.next_btn.clicked.connect(lambda: self._change_page(1))
        footer_layout.addWidget(self.page_label, 1)
        footer_layout.addWidget(self.prev_btn)
        footer_layout.addWidget(self.next_btn)
        card.addWidget(footer)
        self.addCard(card)

    def set_profile(self, profile_id: str, pet_name: str) -> None:
        self._profile_id = profile_id
        self._pet_name = pet_name
        self._page = 0
        self.refresh_data()

    def showEvent(self, event) -> None:
        super().showEvent(event)
        self.refresh_data()
        self._poll_timer.start()

    def hideEvent(self, event) -> None:
        self._poll_timer.stop()
        super().hideEvent(event)

    def _on_pool_preset_changed(self) -> None:
        pool = self.pool_combo.currentData()
        self.partition_combo.blockSignals(True)
        self.status_combo.blockSignals(True)
        self.sort_combo.blockSignals(True)
        if pool == "active_pool":
            self.partition_combo.setCurrentIndex(0)
            self.status_combo.setCurrentIndex(1)  # all statuses in snapshot
            self.sort_combo.setCurrentIndex(4)    # activation
        elif pool == "hippocampus":
            self.partition_combo.setCurrentIndex(
                max(0, self.partition_combo.findData("hippocampus")))
            self.status_combo.setCurrentIndex(0)  # active_only
            self.sort_combo.setCurrentIndex(1)    # updated
        elif pool == "important":
            self.partition_combo.setCurrentIndex(0)
            self.status_combo.setCurrentIndex(0)  # active_only
            self.sort_combo.setCurrentIndex(0)    # importance
        elif pool == "cortical":
            self.partition_combo.setCurrentIndex(0)
            self.status_combo.setCurrentIndex(0)  # active_only
            self.sort_combo.setCurrentIndex(0)    # importance
        else:
            self.partition_combo.setCurrentIndex(0)
            self.status_combo.setCurrentIndex(0)
            self.sort_combo.setCurrentIndex(1)
        self.partition_combo.blockSignals(False)
        self.status_combo.blockSignals(False)
        self.sort_combo.blockSignals(False)
        self._page = 0
        self.refresh_data()

    def _on_filter_changed(self, *_args) -> None:
        self._page = 0
        self.refresh_data()

    def _change_page(self, delta: int) -> None:
        self._page = max(0, self._page + delta)
        self.refresh_data()

    def _db_path(self):
        if not self._profile_id:
            return None
        try:
            path = profile_directory(self._profile_id) / "memory.db"
            return path if path.is_file() else None
        except ValueError:
            return None

    def refresh_data(self) -> None:
        db_path = self._db_path()
        if db_path is None:
            self.status_label.setText(f"{self._pet_name or '未选择角色'} · 尚未生成 memory.db")
            self._current_rows = []
            self.table.setRowCount(0)
            self.empty_label.show()
            self.table.hide()
            return

        self.status_label.setText(f"{self._pet_name or '当前角色'} · 数据库：{db_path}")
        try:
            conn = sqlite3.connect(str(db_path), timeout=1.0)
            conn.row_factory = sqlite3.Row
            try:
                self._load_summary(conn)
                self._load_rows(conn)
            finally:
                conn.close()
        except sqlite3.Error as error:
            self.status_label.setText(f"读取 memory.db 失败：{error}")

    def _load_summary(self, conn: sqlite3.Connection) -> None:
        active_pool_cnt = conn.execute(
            "SELECT count(*) FROM memory_activation_snapshots"
        ).fetchone()[0]
        hippo_active = conn.execute(
            "SELECT count(*) FROM memory_items WHERE partition='hippocampus' AND status IN ('active','consolidated')"
        ).fetchone()[0]
        cortical_active = conn.execute(
            "SELECT count(*) FROM memory_items WHERE partition!='hippocampus' AND status IN ('active','consolidated')"
        ).fetchone()[0]
        high_imp = conn.execute(
            "SELECT count(*) FROM memory_items WHERE importance>=0.7 AND status IN ('active','consolidated')"
        ).fetchone()[0]
        rel_cnt = conn.execute("SELECT count(*) FROM memory_relations").fetchone()[0]
        total_cnt = conn.execute("SELECT count(*) FROM memory_items").fetchone()[0]

        self.counter_labels["active_pool"].setText(f"{active_pool_cnt:,}")
        self.counter_labels["hippocampus_active"].setText(f"{hippo_active:,}")
        self.counter_labels["long_term_active"].setText(f"{cortical_active:,}")
        self.counter_labels["high_importance"].setText(f"{high_imp:,}")
        self.counter_labels["relations"].setText(f"{rel_cnt:,}")
        self.counter_labels["total"].setText(f"{total_cnt:,}")

        parts = []
        for row in conn.execute(
            "SELECT partition, status, count(*) AS cnt FROM memory_items "
            "GROUP BY partition, status ORDER BY partition, status"
        ):
            parts.append(f"{row['partition'] or '未分区'}[{row['status']}]: {row['cnt']}")
        self.partition_breakdown_label.setText(
            "分区状态分布：" + ("  ·  ".join(parts) if parts else "暂无数据")
        )

    def _load_rows(self, conn: sqlite3.Connection) -> None:
        pool = self.pool_combo.currentData()
        partition = self.partition_combo.currentData()
        status_filter = self.status_combo.currentData()
        sort_key = self.sort_combo.currentData()
        keyword = self.search_edit.text().strip().lower()

        tags_by_id: dict[str, list[str]] = {}
        for r in conn.execute("SELECT memory_id, tag FROM memory_tags"):
            tags_by_id.setdefault(r["memory_id"], []).append(r["tag"])

        activations: dict[str, dict] = {}
        for r in conn.execute("SELECT * FROM memory_activation_snapshots"):
            activations[r["memory_id"]] = dict(r)

        rows = []
        for r in conn.execute("SELECT * FROM memory_items"):
            item = dict(r)
            mid = item["id"]
            item_tags = tags_by_id.get(mid, [])
            act = activations.get(mid)
            item["_tags"] = item_tags
            item["_activation"] = float(act["activation"]) if act else 0.0
            item["_in_active_pool"] = act is not None

            if pool == "active_pool" and not item["_in_active_pool"]:
                continue
            if pool == "hippocampus" and item.get("partition") != "hippocampus":
                continue
            if pool == "cortical" and item.get("partition") == "hippocampus":
                continue
            if pool == "important" and float(item.get("importance") or 0.0) < 0.5:
                continue

            if partition and item.get("partition") != partition:
                continue
            if status_filter == "active_only":
                if item.get("status") not in ("active", "consolidated"):
                    continue
            elif status_filter and item.get("status") != status_filter:
                continue

            if keyword:
                haystack = " ".join([
                    str(item.get("key") or ""),
                    str(item.get("summary") or ""),
                    str(item.get("content") or ""),
                    str(item.get("type") or ""),
                    " ".join(item_tags),
                ]).lower()
                if keyword not in haystack:
                    continue

            rows.append(item)

        if sort_key == "importance":
            rows.sort(
                key=lambda x: (
                    float(x.get("importance") or 0.0),
                    float(x.get("strength") or 0.0),
                    str(x.get("updated_at") or ""),
                ),
                reverse=True,
            )
        elif sort_key == "strength":
            rows.sort(
                key=lambda x: (
                    float(x.get("strength") or 0.0),
                    float(x.get("importance") or 0.0),
                    str(x.get("updated_at") or ""),
                ),
                reverse=True,
            )
        elif sort_key == "access":
            rows.sort(
                key=lambda x: (
                    int(x.get("access_count") or 0) + int(x.get("mention_count") or 0),
                    float(x.get("importance") or 0.0),
                    str(x.get("updated_at") or ""),
                ),
                reverse=True,
            )
        elif sort_key == "activation":
            rows.sort(
                key=lambda x: (
                    float(x.get("_activation") or 0.0),
                    float(x.get("importance") or 0.0),
                    str(x.get("updated_at") or ""),
                ),
                reverse=True,
            )
        else:
            rows.sort(
                key=lambda x: str(x.get("updated_at") or x.get("created_at") or ""),
                reverse=True,
            )

        total = len(rows)
        pages = max(1, math.ceil(total / self.PAGE_SIZE))
        self._page = min(self._page, pages - 1)
        page_rows = rows[self._page * self.PAGE_SIZE:(self._page + 1) * self.PAGE_SIZE]
        self._current_rows = page_rows

        self.table.setUpdatesEnabled(False)
        self.table.setRowCount(len(page_rows))
        for row_idx, item in enumerate(page_rows):
            part_type = f"{item.get('partition') or '—'} / {item.get('type') or ''}"
            status = str(item.get("status") or "")
            imp = f"{float(item.get('importance') or 0.0):.2f}"
            strength_act = f"{float(item.get('strength') or 0.0):.2f}"
            if item.get("_in_active_pool"):
                strength_act += f" (激活 {item['_activation']:.2f})"
            counts = f"{int(item.get('access_count') or 0)} / {int(item.get('mention_count') or 0)}"
            tags_str = ", ".join(item.get("_tags") or [])
            text = str(item.get("summary") or item.get("content") or item.get("key") or "").replace("\n", " ")
            updated = format_time(str(item.get("updated_at") or item.get("created_at") or ""), short=True)

            cells = (part_type, status, imp, strength_act, counts, tags_str, text, updated)
            for col_idx, val in enumerate(cells):
                cell = QTableWidgetItem(str(val))
                cell.setToolTip(f"{val}\n（点击查看完整记忆详情）")
                self.table.setItem(row_idx, col_idx, cell)
        self.table.setUpdatesEnabled(True)

        self.page_label.setText(f"{self._page + 1} / {pages} 页 · 共 {total} 条匹配记忆（点击任意行查看详情）")
        self.prev_btn.setEnabled(self._page > 0)
        self.next_btn.setEnabled(self._page + 1 < pages)
        self.empty_label.setVisible(not page_rows)
        self.table.setVisible(bool(page_rows))

    def _on_row_clicked(self, row: int, _col: int = 0) -> None:
        if not (0 <= row < len(self._current_rows)):
            return
        item = self._current_rows[row]
        mid = item["id"]
        db_path = self._db_path()
        if db_path is None:
            return
        detail = {"item": item, "tags": item.get("_tags", [])}
        try:
            conn = sqlite3.connect(str(db_path), timeout=1.0)
            conn.row_factory = sqlite3.Row
            try:
                act = conn.execute(
                    "SELECT * FROM memory_activation_snapshots WHERE memory_id=?", (mid,)
                ).fetchone()
                if act:
                    detail["activation"] = dict(act)
                attr = conn.execute(
                    "SELECT * FROM memory_attribution WHERE memory_id=?", (mid,)
                ).fetchone()
                if attr:
                    detail["attribution"] = dict(attr)
                detail["people"] = [
                    dict(r) for r in conn.execute(
                        "SELECT person_id, role FROM memory_people WHERE memory_id=?", (mid,)
                    )
                ]
                detail["origins"] = [
                    dict(r) for r in conn.execute(
                        "SELECT origin_id, conversation_id, speaker_id, original_at, evidence_ref, source_status "
                        "FROM memory_origins WHERE memory_id=?",
                        (mid,),
                    )
                ]
                detail["evidence"] = [
                    dict(r) for r in conn.execute(
                        "SELECT source, raw_text, created_at FROM memory_evidence WHERE memory_id=?",
                        (mid,),
                    )
                ]
                detail["relations"] = [
                    dict(r) for r in conn.execute(
                        "SELECT * FROM memory_relations WHERE from_memory_id=? OR to_memory_id=?",
                        (mid, mid),
                    )
                ]
                detail["access_logs"] = [
                    dict(r) for r in conn.execute(
                        "SELECT reason, score, created_at FROM memory_access_log "
                        "WHERE memory_id=? ORDER BY created_at DESC LIMIT 20",
                        (mid,),
                    )
                ]
            finally:
                conn.close()
        except sqlite3.Error:
            pass

        dialog = MemoryDetailDialog(detail, self.window())
        dialog.open()
