"""DesktopPet Python 启动器入口。

功能（见前端修改计划 §五）：
1. Fluent Design 主框架 + 导航各设置页；
2. 收集各页配置 → config_loader 导出 launch_config.json；
3. 主题与 C++ 经同名 QSettings (键 ui/theme) 共享 —— 关键：org/app 名逐字对齐 C++。
4. 启动 C++ 核心：Desktop_Pet --config <abs> --pet <name>。
"""

from __future__ import annotations

import os
import sys
import subprocess

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from PySide6.QtCore import Qt, QSettings
from PySide6.QtWidgets import QApplication
from qfluentwidgets import FluentIcon as FIF, FluentWindow, NavigationItemPosition, PushButton, \
    InfoBar, InfoBarPosition

from app_state import AppState
import config_loader
from pet_registry import ORG_NAME, APP_NAME  # 逐字对齐 C++ main.cpp
from pages.pet_page import PetPage
from pages.ai_page import AiPage
from pages.voice_page import VoicePage
from pages.bubble_page import BubblePage
from pages.advanced_page import AdvancedPage
from pages.about_page import AboutPage

# C++ 可执行文件位置（构建产物）
CPP_EXECUTABLE = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "build", "Desktop_Pet",
)

THEME_KEY = "ui/theme"  # 与 C++ ThemeManager 逐字一致


class LauncherWindow(FluentWindow):
    def __init__(self):
        super().__init__()
        self.state = AppState()
        self.setWindowTitle("DesktopPet 启动器")
        self.resize(900, 640)

        # 主题：从共享 QSettings 读取并应用
        self._apply_theme(self._read_theme())

        self.pet_page = PetPage(self.state)
        self.ai_page = AiPage(self.state)
        self.voice_page = VoicePage(self.state)
        self.bubble_page = BubblePage(self.state)
        self.advanced_page = AdvancedPage(self.state)
        self.about_page = AboutPage()

        self.addSubInterface(self.pet_page, FIF.HOME, "宠物")
        self.addSubInterface(self.ai_page, FIF.ROBOT, "AI")
        self.addSubInterface(self.voice_page, FIF.MEGAPHONE, "语音")
        self.addSubInterface(self.bubble_page, FIF.CHAT, "气泡")
        self.addSubInterface(self.advanced_page, FIF.SETTING, "高级")
        self.addSubInterface(
            self.about_page, FIF.INFO, "关于", NavigationItemPosition.BOTTOM)

        # 导航栏底部「启动宠物」按钮
        self.start_btn = PushButton(FIF.PLAY, "启动宠物")
        self.start_btn.setFixedHeight(40)
        self.navigationInterface.addWidget(
            "startButton", self.start_btn, lambda: self._on_start())
        # 主题切换按钮
        self.theme_btn = PushButton(FIF.BRIGHTNESS, "切换主题")
        self.theme_btn.setFixedHeight(40)
        self.navigationInterface.addWidget(
            "themeButton", self.theme_btn, lambda: self._toggle_theme())

    # —— 主题（与 C++ 共享 QSettings）——
    def _read_theme(self) -> str:
        # QSettings 用 org/app 名定位（已在 QApplication 全局设置）
        return QSettings().value(THEME_KEY, "dark", type=str)

    def _apply_theme(self, theme: str):
        self.state.theme = theme
        try:
            from qfluentwidgets import DarkFluentWindow, FluentWindow as _FW
            from qfluentwidgets import setTheme, Theme  # type: ignore
            setTheme(Theme.DARK if theme == "dark" else Theme.LIGHT)
        except Exception:
            pass

    def _toggle_theme(self):
        new_theme = "light" if self.state.theme == "dark" else "dark"
        QSettings().setValue(THEME_KEY, new_theme)
        self._apply_theme(new_theme)

    # —— 启动 C++ 核心 ——
    def _on_start(self):
        if not self.state.pet_name:
            InfoBar.warning("未选择角色", "请先在「宠物」页选择一个角色",
                            parent=self, position=InfoBarPosition.TOP, duration=3000)
            return
        if not os.path.exists(CPP_EXECUTABLE):
            InfoBar.error("找不到核心", f"未找到可执行文件：{CPP_EXECUTABLE}\n请先构建 C++ 部分",
                          parent=self, position=InfoBarPosition.TOP, duration=5000)
            return

        try:
            # 1) 模板 + 用户字段覆盖 + 导出
            template = config_loader.load_template()
            cfg = config_loader.apply_settings(template, self.state.to_settings_dict())
            # 高级设置直接覆盖模板副本对应字段
            overrides = self.state.to_advanced_overrides()
            cfg["renderSettings"].update(overrides["renderSettings"])
            cfg["interactionSettings"]["dragThreshold"] = overrides["interactionSettings"]["dragThreshold"]
            cfg["interactionSettings"]["clickTimeout"] = overrides["interactionSettings"]["clickTimeout"]
            cfg["interactionSettings"]["windowSnapping"].update(
                overrides["interactionSettings"]["windowSnapping"])
            config_path = config_loader.export_config(cfg)
        except Exception as e:
            InfoBar.error("导出配置失败", str(e),
                          parent=self, position=InfoBarPosition.TOP, duration=5000)
            return

        # 2) 启动 C++ 核心（绝对路径 --config）
        try:
            args = [
                CPP_EXECUTABLE,
                "--config", config_path,  # 绝对路径，避开 QDir::setCurrent 歧义
                "--pet", self.state.pet_name,
            ]
            subprocess.Popen(args, start_new_session=True)
        except OSError as e:
            InfoBar.error("启动失败", str(e),
                          parent=self, position=InfoBarPosition.TOP, duration=5000)
            return

        InfoBar.success("已启动", f"已唤起 {self.state.pet_name}（核心进程已启动）",
                        parent=self, position=InfoBarPosition.TOP, duration=3000)


def main():
    # 关键：org/app 名逐字对齐 C++ main.cpp，QSettings(键 ui/theme) 路径才一致。
    QApplication.setOrganizationName(ORG_NAME)
    QApplication.setApplicationName(APP_NAME)
    QApplication.setApplicationVersion("1.0.0")

    app = QApplication(sys.argv)
    window = LauncherWindow()
    window.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()