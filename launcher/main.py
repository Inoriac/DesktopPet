"""DesktopPet Python 启动器入口。

功能（见前端修改计划 §五）：
1. Fluent Design 主框架 + 导航各设置页（对齐官方参考示例 demo.py：
   MSFluentWindow + 自定义标题栏 + 卡片分区页）；
2. 收集各页配置 → config_loader 导出 launch_config.json；
3. 主题与 C++ 经同名 QSettings (键 ui/theme) 共享 —— 关键：org/app 名逐字对齐 C++。
4. 启动 C++ 核心：Desktop_Pet --config <abs> --pet <name>。
"""

from __future__ import annotations

import os
import sys
import subprocess

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from PySide6.QtCore import Qt, QSize, QSettings, QByteArray
from PySide6.QtGui import QIcon, QPixmap, QColor, QPainter
from PySide6.QtSvg import QSvgRenderer
from PySide6.QtWidgets import QApplication
from qfluentwidgets import (
    FluentIcon as FIF, MSFluentWindow, MSFluentTitleBar, NavigationItemPosition,
    NavigationPushButton, SearchLineEdit, InfoBar, InfoBarPosition,
)

from app_state import AppState
import config_loader
from pet_registry import ORG_NAME, APP_NAME  # 逐字对齐 C++ main.cpp
from pages.pet_page import PetPage
from pages.ai_page import AiPage
from pages.voice_page import VoicePage
from pages.bubble_page import BubblePage
from pages.advanced_page import AdvancedPage
from pages.about_page import AboutPage

# C++ 可执行文件位置（构建产物）。Windows 下产物带 .exe 后缀。
_EXE_SUFFIX = ".exe" if sys.platform == "win32" else ""
CPP_EXECUTABLE = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "build", f"Desktop_Pet{_EXE_SUFFIX}",
)

THEME_KEY = "ui/theme"  # 与 C++ ThemeManager 逐字一致


# —— 太阳 / 月亮 线条图标（简约 SVG，无内置 SUN/MOON FluentIcon，故自绘）——
_SUN_SVG = (
    '<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" '
    'fill="none" stroke="{c}" stroke-width="1.6" stroke-linecap="round">'
    '<circle cx="12" cy="12" r="4.2"/>'
    '<line x1="12" y1="2.5" x2="12" y2="5"/><line x1="12" y1="19" x2="12" y2="21.5"/>'
    '<line x1="2.5" y1="12" x2="5" y2="12"/><line x1="19" y1="12" x2="21.5" y2="12"/>'
    '<line x1="5.2" y1="5.2" x2="6.9" y2="6.9"/><line x1="17.1" y1="17.1" x2="18.8" y2="18.8"/>'
    '<line x1="18.8" y1="5.2" x2="17.1" y2="6.9"/><line x1="6.9" y1="17.1" x2="5.2" y2="18.8"/>'
    '</svg>'
)
_MOON_SVG = (
    '<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" '
    'fill="none" stroke="{c}" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round">'
    '<path d="M20 14.5A8 8 0 1 1 9.5 4 a6.5 6.5 0 0 0 10.5 10.5 z"/>'
    '</svg>'
)


def _line_icon(svg: str, color: str, size: int = 24) -> QIcon:
    """从 SVG 字符串渲染一张线条 QIcon（自定义太阳/月亮用）。"""
    pm = QPixmap(size, size)
    pm.fill(Qt.transparent)
    renderer = QSvgRenderer(QByteArray(svg.format(c=color).encode("utf-8")))
    p = QPainter()
    p.begin(pm)
    p.setRenderHint(QPainter.Antialiasing)
    renderer.render(p)
    p.end()
    return QIcon(pm)


def _theme_glyph_icon(theme: str, size: int = 36) -> QIcon:
    """返回代表当前主题的图标：dark→月亮，light→太阳。配色随主题取对比灰。"""
    color = "#E0E0E0" if theme == "dark" else "#5B5B5B"
    return _line_icon(_MOON_SVG, color, size) if theme == "dark" \
        else _line_icon(_SUN_SVG, color, size)


class TitleBar(MSFluentTitleBar):
    """对齐 demo.py 的自定义标题栏：居中搜索框。"""

    def __init__(self, parent):
        super().__init__(parent)
        self.searchBox = SearchLineEdit(self)
        self.searchBox.setPlaceholderText("搜索设置、角色、AI、语音…")
        self.searchBox.setClearButtonEnabled(True)
        self.searchBox.setFixedWidth(420)

    def resizeEvent(self, e):
        w, h = self.width(), self.height()
        self.searchBox.move(w // 2 - self.searchBox.width() // 2,
                            h // 2 - self.searchBox.height() // 2)


class LauncherWindow(MSFluentWindow):
    def __init__(self):
        super().__init__()
        self.state = AppState()
        self.setTitleBar(TitleBar(self))
        self.setWindowTitle("DesktopPet 启动器")
        self.resize(1000, 720)

        # 主题：从共享 QSettings 读取并应用
        self._apply_theme(self._read_theme())

        # 宠物页为首页，承载底部「启动桌宠」主按钮
        self.pet_page = PetPage(self.state, on_start=self._on_start)
        self.ai_page = AiPage(self.state)
        self.voice_page = VoicePage(self.state)
        self.bubble_page = BubblePage(self.state)
        self.advanced_page = AdvancedPage(self.state)
        self.about_page = AboutPage()

        # ⚠️ MSFluentWindow.addSubInterface 第 4 个位置参数是 selectedIcon
        #    而非 position；底部项必须用 position= 关键字传，否则枚举会被
        #    当成 QIcon 在选中态绘制时崩溃（即「点击关于闪退」的根因）。
        self.addSubInterface(self.pet_page, FIF.HOME, "宠物",
                             selectedIcon=FIF.HOME_FILL)
        self.addSubInterface(self.ai_page, FIF.ROBOT, "AI")
        self.addSubInterface(self.voice_page, FIF.MEGAPHONE, "语音")
        self.addSubInterface(self.bubble_page, FIF.CHAT, "气泡")
        self.addSubInterface(self.advanced_page, FIF.SETTING, "高级")

        # 主题切换项：放「关于」之上（底部区，先于关于加入即排在关于上方）。
        # 不可选中（isSelectable=False），点击切换主题不切换页面。
        # ⚠️ MSFluentWindow 的 navigationInterface 是 NavigationBar，其 addWidget
        #    不支持 tooltip 参数，故 tooltip 直接设在按钮上。
        self.theme_nav_btn = NavigationPushButton(
            _theme_glyph_icon(self.state.theme, size=36), "切换主题",
            isSelectable=False, parent=self)
        self.theme_nav_btn.setToolTip("切换主题")
        self.navigationInterface.addWidget(
            "themeButton", self.theme_nav_btn,
            onClick=lambda: self._toggle_theme(),
            position=NavigationItemPosition.BOTTOM)
        self.addSubInterface(
            self.about_page, FIF.INFO, "关于", position=NavigationItemPosition.BOTTOM)

        # 内容区透明，让卡片背景自然呈现（对齐 demo.py）
        self.stackedWidget.setStyleSheet("QWidget{background: transparent}")

    # —— 主题（与 C++ 共享 QSettings）——
    def _read_theme(self) -> str:
        return QSettings().value(THEME_KEY, "dark", type=str)

    def _apply_theme(self, theme: str):
        self.state.theme = theme
        try:
            from qfluentwidgets import setTheme, Theme  # type: ignore
            setTheme(Theme.DARK if theme == "dark" else Theme.LIGHT)
        except Exception:
            pass
        self._sync_theme_glyph()

    def _sync_theme_glyph(self):
        """按当前主题刷新导航栏主题按钮的太阳/月亮图标与提示。"""
        btn = getattr(self, "theme_nav_btn", None)
        if btn is None:
            return
        btn._icon = _theme_glyph_icon(self.state.theme)
        btn.update()  # 重绘以显示新图标
        btn.setToolTip(
            f"切换主题（当前：{'深色' if self.state.theme == 'dark' else '浅色'}）")

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

        # 2) 启动 C++ 核心（绝对路径 --config）。detach 语义跨平台：
        #    POSIX 用 start_new_session（新进程组/会话）；Windows 用
        #    DETACHED_PROCESS|CREATE_NEW_PROCESS_GROUP，否则子进程可能随父退出。
        try:
            args = [
                CPP_EXECUTABLE,
                "--config", config_path,  # 绝对路径，避开 QDir::setCurrent 歧义
                "--pet", self.state.pet_name,
            ]
            if sys.platform == "win32":
                DETACHED_PROCESS = 0x00000008
                CREATE_NEW_PROCESS_GROUP = 0x00000200
                subprocess.Popen(
                    args,
                    creationflags=DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
                    close_fds=True)
            else:
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