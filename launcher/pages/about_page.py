"""关于页：项目名 / 版本 / 作者 / 许可证 / GitHub。

布局对齐 demo.py：SingleDirectionScrollArea 内置 SimpleCardWidget 信息卡。
"""

from __future__ import annotations

import os
import re

from PySide6.QtCore import Qt, QUrl
from PySide6.QtGui import QColor, QFont
from PySide6.QtWidgets import QVBoxLayout
from qfluentwidgets import (
    SimpleCardWidget, TitleLabel, BodyLabel, CaptionLabel, HyperlinkLabel,
    PrimaryPushButton, setFont,
)

from ._ui import ScrollPage

VERSION = "1.0.0"
AUTHOR = "Inoriac"
GITHUB_URL = "https://github.com/Inoriac/DesktopPet"


def _detect_license_name() -> str:
    """从 LICENSE.md 解析许可证名称（取 `licensed under the **X**` 处的 X），失败回退默认。"""
    try:
        license_path = os.path.join(
            os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
            "LICENSE.md",
        )
        with open(license_path, "r", encoding="utf-8") as f:
            text = f.read()
    except OSError:
        return "MateEngine Pro License (v2.1)"
    m = re.search(r"licensed under the\s+\*\*(.+?)\*\*", text, re.IGNORECASE)
    if m:
        return m.group(1).strip()
    return "MateEngine Pro License (v2.1)"


class AboutPage(ScrollPage):
    def __init__(self, parent=None):
        super().__init__("AboutPage", parent)
        self.addCard(self._build_info_card(), stretch=0)
        self.addCard(self._build_license_card(), stretch=0)
        self.addStretch()

    def _build_info_card(self) -> SimpleCardWidget:
        card = SimpleCardWidget(self)
        v = QVBoxLayout(card)
        v.setContentsMargins(34, 24, 24, 24)
        v.setSpacing(8)

        name = TitleLabel("DesktopPet", card)
        v.addWidget(name)

        ver = BodyLabel(f"版本 {VERSION}", card)
        v.addWidget(ver)

        author = CaptionLabel(f"作者 {AUTHOR}", card)
        author.setTextColor(QColor(96, 96, 96), QColor(206, 206, 206))
        v.addWidget(author)
        v.addSpacing(6)

        link = HyperlinkLabel(QUrl(GITHUB_URL), "GitHub 仓库", card)
        v.addWidget(link)
        v.addSpacing(10)

        github_btn = PrimaryPushButton("打开 GitHub 仓库", card)
        github_btn.setFixedWidth(200)
        github_btn.clicked.connect(self._open_github)
        v.addWidget(github_btn, 0, Qt.AlignLeft)
        return card

    def _build_license_card(self) -> SimpleCardWidget:
        card = SimpleCardWidget(self)
        v = QVBoxLayout(card)
        v.setContentsMargins(34, 24, 24, 24)
        v.setSpacing(6)

        title = BodyLabel("许可证", card)
        setFont(title, 16, QFont.DemiBold)
        v.addWidget(title)

        # 仅显示协议名称，不展开全文（全文见仓库 LICENSE.md / GitHub）
        name = BodyLabel(_detect_license_name(), card)
        v.addWidget(name)

        note = CaptionLabel("开源非商业许可 · 完整文本见仓库 LICENSE.md", card)
        note.setTextColor(QColor(96, 96, 96), QColor(206, 206, 206))
        v.addWidget(note)

        v.addSpacing(4)
        license_link = HyperlinkLabel(QUrl(GITHUB_URL + "/blob/master/LICENSE.md"),
                                      "查看完整许可证", card)
        v.addWidget(license_link)
        return card

    @staticmethod
    def _open_github():
        from PySide6.QtGui import QDesktopServices
        QDesktopServices.openUrl(QUrl(GITHUB_URL))