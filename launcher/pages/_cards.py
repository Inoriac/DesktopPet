"""Fluent SettingCard 构造助手。

qfluentwidgets 的 SettingCard 签名是 (icon, title, content, parent)，
自定义控件需通过 card.hBoxLayout.addWidget(ctrl, 0, Qt.AlignRight) 塞入右侧。
这里把该模式收口为 make_card()，供各页面复用。
"""

from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QWidget
from qfluentwidgets import SettingCard, FluentIcon as FIF

# 各页面可复用的图标（避免每个文件各自 import FluentIcon）
_ICONS = {
    "default": FIF.COMMAND_PROMPT,
    "pet": FIF.HOME,
    "ai": FIF.ROBOT,
    "voice": FIF.MEGAPHONE,
    "bubble": FIF.CHAT,
    "gear": FIF.SETTING,
}


def make_card(ctrl: QWidget, title: str, content: str = "", icon_key: str = "default") -> SettingCard:
    """构造一个右侧带 ctrl 控件、左带图标/标题/说明的 SettingCard。"""
    card = SettingCard(_ICONS.get(icon_key, FIF.COMMAND_PROMPT), title, content)
    card.hBoxLayout.addWidget(ctrl, 0, Qt.AlignRight)
    return card