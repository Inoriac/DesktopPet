"""Read-only owner diary page backed exclusively by OwnerDiaryClient."""

from __future__ import annotations

import json

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QHBoxLayout, QListWidget, QListWidgetItem, QTextBrowser, QVBoxLayout, QWidget,
)
from qfluentwidgets import CaptionLabel, FluentIcon as FIF, PushButton

from owner_diary_client import OwnerDiaryClient, OwnerDiaryError
from ._ui import ScrollPage, Section


class PrivateDiaryPage(ScrollPage):
    PAGE_SIZE = 30

    def __init__(self, parent=None):
        super().__init__("PrivateDiaryPage", parent)
        self._client: OwnerDiaryClient | None = None
        self._entries_by_id: dict[str, dict] = {}
        self._next_cursor = ""
        self._active = False

        diary_section = Section("私人日记", self)
        toolbar = QWidget(diary_section)
        toolbar_layout = QHBoxLayout(toolbar)
        toolbar_layout.setContentsMargins(0, 0, 0, 0)
        self.status_label = CaptionLabel("离线", toolbar)
        self.refresh_button = PushButton(FIF.SYNC, "刷新", toolbar)
        self.more_button = PushButton(FIF.DOWNLOAD, "加载更多", toolbar)
        self.refresh_button.clicked.connect(self.refresh_entries)
        self.more_button.clicked.connect(self.load_more)
        toolbar_layout.addWidget(self.status_label)
        toolbar_layout.addStretch(1)
        toolbar_layout.addWidget(self.refresh_button)
        toolbar_layout.addWidget(self.more_button)
        diary_section.addWidget(toolbar)

        content = QWidget(diary_section)
        content_layout = QHBoxLayout(content)
        content_layout.setContentsMargins(0, 0, 0, 0)
        content_layout.setSpacing(16)
        self.entry_list = QListWidget(content)
        self.entry_list.setMinimumWidth(260)
        self.entry_list.setMaximumWidth(360)
        self.entry_list.itemActivated.connect(
            lambda _item: self.load_selected_entry())
        self.body_view = QTextBrowser(content)
        self.body_view.setOpenExternalLinks(False)
        content_layout.addWidget(self.entry_list, 0)
        content_layout.addWidget(self.body_view, 1)
        diary_section.addWidget(content)
        self.addCard(diary_section)
        self.addStretch()
        self._sync_controls()

    def set_client(self, client: OwnerDiaryClient | None) -> None:
        was_active = self._active
        self.deactivate()
        previous = self._client
        if previous is not None:
            try:
                previous.disconnected.disconnect(self._on_client_disconnected)
            except (RuntimeError, TypeError):
                pass
        self._client = client
        if client is not None:
            client.disconnected.connect(self._on_client_disconnected)
        self._active = was_active
        self._sync_controls()
        if self._active:
            self.refresh_entries()

    def activate(self) -> None:
        self._active = True
        self.refresh_entries()

    def deactivate(self) -> None:
        self._active = False
        self.body_view.clear()

    def refresh_entries(self) -> None:
        self.entry_list.clear()
        self._entries_by_id.clear()
        self._next_cursor = ""
        self._load_page(None)

    def load_more(self) -> None:
        if self._next_cursor:
            self._load_page(self._next_cursor)

    def load_selected_entry(self) -> None:
        item = self.entry_list.currentItem()
        if item is None or self._client is None or not self._client.is_connected:
            self.body_view.clear()
            self._set_status("离线")
            return
        entry_id = item.data(Qt.UserRole)
        try:
            entry = self._client.get_entry(str(entry_id))
        except OwnerDiaryError:
            self.body_view.clear()
            self._set_status(
                "日记不可读" if self._client.is_connected else "离线")
            self._sync_controls()
            return
        self.body_view.setPlainText(str(entry.get("body", "")))
        self._set_status(str(entry.get("localDate", "")))

    def showEvent(self, event):
        super().showEvent(event)
        self.activate()

    def hideEvent(self, event):
        self.deactivate()
        super().hideEvent(event)

    def _load_page(self, cursor: str | None) -> None:
        if self._client is None or not self._client.is_connected:
            self._set_status("离线")
            self._sync_controls()
            return
        try:
            page = self._client.list_entries("", "", cursor, self.PAGE_SIZE)
        except OwnerDiaryError:
            self.body_view.clear()
            self._set_status("离线")
            self._sync_controls()
            return
        entries = page.get("entries", [])
        if not isinstance(entries, list):
            entries = []
        for entry in entries:
            if not isinstance(entry, dict):
                continue
            entry_id = str(entry.get("entryId", ""))
            local_date = str(entry.get("localDate", ""))
            if not entry_id or not local_date:
                continue
            index = entry.get("index") if isinstance(entry.get("index"), dict) else {}
            summary = json.dumps(index, ensure_ascii=False, separators=(",", ":"))
            text = local_date if not summary or summary == "{}" \
                else f"{local_date}\n{summary}"
            item = QListWidgetItem(text, self.entry_list)
            item.setData(Qt.UserRole, entry_id)
            self._entries_by_id[entry_id] = entry
        self._next_cursor = str(page.get("nextCursor") or "")
        self._set_status("暂无日记" if self.entry_list.count() == 0 else "已连接")
        self._sync_controls()

    def _set_status(self, text: str) -> None:
        self.status_label.setText(text)

    def _on_client_disconnected(self) -> None:
        self.body_view.clear()
        self._set_status("离线")
        self._sync_controls()

    def _sync_controls(self) -> None:
        connected = self._client is not None and self._client.is_connected
        self.refresh_button.setEnabled(connected)
        self.more_button.setEnabled(connected and bool(self._next_cursor))
