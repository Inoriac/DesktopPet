"""关于页：项目名 / 版本 / 作者 / 许可证 / GitHub。"""

from __future__ import annotations

import os

from PySide6.QtWidgets import QWidget, QVBoxLayout
from qfluentwidgets import ScrollArea, BodyLabel, TitleLabel, PushButton

VERSION = "1.0.0"
AUTHOR = "Inoriac"
GITHUB_URL = "https://github.com/Inoriac/DesktopPet"


class AboutPage(ScrollArea):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setObjectName("AboutPage")

        container = QWidget()
        layout = QVBoxLayout(container)
        layout.setContentsMargins(48, 32, 48, 32)
        layout.setSpacing(10)

        layout.addWidget(TitleLabel("DesktopPet"))
        layout.addWidget(BodyLabel(f"版本：{VERSION}"))
        layout.addWidget(BodyLabel(f"作者：{AUTHOR}"))

        # 许可证（显示 LICENSE.md 内容前若干行）
        license_path = os.path.join(
            os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
            "LICENSE.md",
        )
        license_text = "（未找到 LICENSE.md）"
        try:
            with open(license_path, "r", encoding="utf-8") as f:
                license_text = f.read().strip()[:4000]
        except OSError:
            pass
        layout.addSpacing(8)
        layout.addWidget(BodyLabel("许可证："))
        layout.addWidget(BodyLabel(license_text))

        layout.addSpacing(8)
        github_btn = PushButton("打开 GitHub 仓库")
        github_btn.clicked.connect(self._open_github)
        layout.addWidget(github_btn)

        layout.addStretch(1)
        self.setWidget(container)
        self.setWidgetResizable(True)

    @staticmethod
    def _open_github():
        from PySide6.QtGui import QDesktopServices
        from PySide6.QtCore import QUrl
        QDesktopServices.openUrl(QUrl(GITHUB_URL))