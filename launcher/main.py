"""DesktopPet Python 启动器入口。

功能（见前端修改计划 §五）：
1. Fluent Design 主框架 + 导航各设置页（对齐官方参考示例 demo.py：
   MSFluentWindow + 自定义标题栏 + 卡片分区页）；
2. 收集各页配置 → config_loader 导出 launch_config.json；
3. 主题与 C++ 经同名 QSettings (键 ui/theme) 共享 —— 关键：org/app 名逐字对齐 C++。
4. 启动 C++ 核心：Desktop_Pet --config <abs> --pet <name> --profile-id <uuid>。
"""

from __future__ import annotations

import os
import sys
import subprocess
import base64
import getpass
import hashlib
import json
import secrets
import tempfile
from datetime import datetime, timedelta, timezone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from PySide6.QtCore import Qt, QSize, QSettings, QByteArray, QTimer
from PySide6.QtGui import QIcon, QPixmap, QColor, QPainter
from PySide6.QtSvg import QSvgRenderer
from PySide6.QtWidgets import QApplication
from qfluentwidgets import (
    FluentIcon as FIF, MSFluentWindow, MSFluentTitleBar, NavigationItemPosition,
    NavigationPushButton, SearchLineEdit, InfoBar, InfoBarPosition,
)

from app_state import AppState
import config_loader
from pet_registry import APP_NAME, ORG_NAME, PetRegistryError, find_pet
from pages.pet_page import PetPage
from pages.ai_page import AiPage
from pages.voice_page import VoicePage
from pages.bubble_page import BubblePage
from pages.advanced_page import AdvancedPage
from pages.about_page import AboutPage
from pages.private_diary_page import PrivateDiaryPage
from owner_diary_client import OwnerDiaryClient, OwnerDiaryError
from process_tracker import PetProcessTracker

# C++ 可执行文件位置（构建产物）。Windows 下产物带 .exe 后缀。
_EXE_SUFFIX = ".exe" if sys.platform == "win32" else ""
_PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CPP_EXECUTABLE = os.environ.get(
    "DESKTOP_PET_EXECUTABLE",
    os.path.join(_PROJECT_ROOT, "build", f"Desktop_Pet{_EXE_SUFFIX}"),
)

THEME_KEY = "ui/theme"  # 与 C++ ThemeManager 逐字一致


def _open_private_bootstrap(path: str) -> int:
    if sys.platform != "win32":
        return os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)

    import ctypes
    import msvcrt
    from ctypes import wintypes

    class SidAndAttributes(ctypes.Structure):
        _fields_ = [("sid", wintypes.LPVOID), ("attributes", wintypes.DWORD)]

    class TokenUser(ctypes.Structure):
        _fields_ = [("user", SidAndAttributes)]

    class SecurityAttributes(ctypes.Structure):
        _fields_ = [
            ("length", wintypes.DWORD),
            ("security_descriptor", wintypes.LPVOID),
            ("inherit_handle", wintypes.BOOL),
        ]

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    advapi32 = ctypes.WinDLL("advapi32", use_last_error=True)
    kernel32.GetCurrentProcess.restype = wintypes.HANDLE
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.CloseHandle.restype = wintypes.BOOL
    kernel32.LocalFree.argtypes = [wintypes.LPVOID]
    kernel32.LocalFree.restype = wintypes.LPVOID
    kernel32.CreateFileW.argtypes = [
        wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
        ctypes.POINTER(SecurityAttributes), wintypes.DWORD,
        wintypes.DWORD, wintypes.HANDLE,
    ]
    kernel32.CreateFileW.restype = wintypes.HANDLE
    advapi32.OpenProcessToken.argtypes = [
        wintypes.HANDLE, wintypes.DWORD, ctypes.POINTER(wintypes.HANDLE)]
    advapi32.OpenProcessToken.restype = wintypes.BOOL
    advapi32.GetTokenInformation.argtypes = [
        wintypes.HANDLE, ctypes.c_int, wintypes.LPVOID, wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD),
    ]
    advapi32.GetTokenInformation.restype = wintypes.BOOL
    advapi32.ConvertSidToStringSidW.argtypes = [
        wintypes.LPVOID, ctypes.POINTER(wintypes.LPWSTR)]
    advapi32.ConvertSidToStringSidW.restype = wintypes.BOOL
    advapi32.ConvertStringSecurityDescriptorToSecurityDescriptorW.argtypes = [
        wintypes.LPCWSTR, wintypes.DWORD, ctypes.POINTER(wintypes.LPVOID),
        ctypes.POINTER(wintypes.DWORD),
    ]
    advapi32.ConvertStringSecurityDescriptorToSecurityDescriptorW.restype = wintypes.BOOL

    token = wintypes.HANDLE()
    if not advapi32.OpenProcessToken(
            kernel32.GetCurrentProcess(), 0x0008, ctypes.byref(token)):
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        token_size = wintypes.DWORD()
        advapi32.GetTokenInformation(token, 1, None, 0, ctypes.byref(token_size))
        if token_size.value == 0:
            raise ctypes.WinError(ctypes.get_last_error())
        token_buffer = ctypes.create_string_buffer(token_size.value)
        if not advapi32.GetTokenInformation(
                token, 1, token_buffer, token_size, ctypes.byref(token_size)):
            raise ctypes.WinError(ctypes.get_last_error())
        user_sid = ctypes.cast(
            token_buffer, ctypes.POINTER(TokenUser)).contents.user.sid
        sid_string = wintypes.LPWSTR()
        if not advapi32.ConvertSidToStringSidW(
                user_sid, ctypes.byref(sid_string)):
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            sddl = f"D:P(A;;GA;;;{sid_string.value})"
        finally:
            kernel32.LocalFree(ctypes.cast(sid_string, wintypes.LPVOID))
    finally:
        kernel32.CloseHandle(token)

    descriptor = wintypes.LPVOID()
    if not advapi32.ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl, 1, ctypes.byref(descriptor), None):
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        attributes = SecurityAttributes(
            ctypes.sizeof(SecurityAttributes), descriptor, False)
        handle = kernel32.CreateFileW(
            path, 0x40000000, 0, ctypes.byref(attributes), 1, 0x100, None)
        invalid_handle = ctypes.c_void_p(-1).value
        if handle == invalid_handle:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            return msvcrt.open_osfhandle(
                handle, os.O_WRONLY | getattr(os, "O_BINARY", 0))
        except Exception:
            kernel32.CloseHandle(handle)
            raise
    finally:
        kernel32.LocalFree(descriptor)


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
        self.state = AppState.from_config(config_loader.load_saved_config())
        self.setTitleBar(TitleBar(self))
        self.setWindowTitle("DesktopPet 启动器")
        self.resize(1000, 720)
        self.owner_diary_client: OwnerDiaryClient | None = None
        self._owner_socket_name: str | None = None
        self._owner_capability_token: str | None = None
        self._owner_bootstrap_path: str | None = None
        self._owner_connect_generation = 0
        self._pet_process_tracker = PetProcessTracker()

        # 主题：从共享 QSettings 读取并应用
        self._apply_theme(self._read_theme())

        # 宠物页为首页，承载底部「启动桌宠」主按钮
        self.pet_page = PetPage(self.state, on_start=self._on_start)
        self.ai_page = AiPage(self.state, on_save=self._on_save_configuration)
        self.voice_page = VoicePage(self.state)
        self.bubble_page = BubblePage(self.state)
        self.advanced_page = AdvancedPage(self.state)
        self.private_diary_page = PrivateDiaryPage()
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
        self.addSubInterface(self.private_diary_page, FIF.DOCUMENT, "私人日记")

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

        self._pet_process_timer = QTimer(self)
        self._pet_process_timer.setInterval(500)
        self._pet_process_timer.timeout.connect(self._refresh_alive_pet_count)
        self._pet_process_timer.start()
        self._refresh_alive_pet_count()

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

    def _refresh_alive_pet_count(self) -> int:
        count = self._pet_process_tracker.refresh()
        self.pet_page.set_alive_count(count)
        return count

    def _export_current_configuration(self) -> str:
        template = config_loader.load_template()
        cfg = config_loader.apply_settings(
            template, self.state.to_settings_dict())
        overrides = self.state.to_advanced_overrides()
        cfg["renderSettings"].update(overrides["renderSettings"])
        cfg["interactionSettings"]["dragThreshold"] = \
            overrides["interactionSettings"]["dragThreshold"]
        cfg["interactionSettings"]["clickTimeout"] = \
            overrides["interactionSettings"]["clickTimeout"]
        cfg["interactionSettings"]["windowSnapping"].update(
            overrides["interactionSettings"]["windowSnapping"])
        return config_loader.export_config(cfg)

    def _on_save_configuration(self) -> None:
        try:
            self._export_current_configuration()
        except Exception as error:
            InfoBar.error(
                "保存失败", str(error), parent=self,
                position=InfoBarPosition.TOP, duration=5000)
            return
        InfoBar.success(
            "配置已保存", "AI 与 API 配置将在下次启动时自动恢复",
            parent=self, position=InfoBarPosition.TOP, duration=3000)

    # —— 启动 C++ 核心 ——
    def _on_start(self):
        if not self.state.pet_name:
            InfoBar.warning("未选择角色", "请先在「宠物」页选择一个角色",
                            parent=self, position=InfoBarPosition.TOP, duration=3000)
            return
        try:
            profile = find_pet(self.state.pet_name)
        except PetRegistryError as error:
            InfoBar.error("角色清单不可用", str(error),
                          parent=self, position=InfoBarPosition.TOP, duration=5000)
            return
        if profile is None:
            InfoBar.warning("未找到角色", "请刷新角色清单后重试",
                            parent=self, position=InfoBarPosition.TOP, duration=3000)
            return
        self.state.pet_profile_id = profile.profile_id
        if not os.path.exists(CPP_EXECUTABLE):
            InfoBar.error("找不到核心", f"未找到可执行文件：{CPP_EXECUTABLE}\n请先构建 C++ 部分",
                          parent=self, position=InfoBarPosition.TOP, duration=5000)
            return

        try:
            # 1) 导出当前配置，启动与显式保存共用同一持久化路径。
            config_path = self._export_current_configuration()
        except Exception as e:
            InfoBar.error("导出配置失败", str(e),
                          parent=self, position=InfoBarPosition.TOP, duration=5000)
            return

        try:
            bootstrap = self._create_owner_diary_bootstrap(profile.profile_id)
        except OSError as error:
            InfoBar.error("私有日记初始化失败", str(error),
                          parent=self, position=InfoBarPosition.TOP, duration=5000)
            return

        self._close_owner_diary_session()
        self._owner_socket_name = bootstrap["socket_name"]
        self._owner_capability_token = bootstrap["capability_token"]
        self._owner_bootstrap_path = bootstrap["path"]
        self._owner_connect_generation += 1
        generation = self._owner_connect_generation

        # 2) 启动 C++ 核心（绝对路径 --config）。detach 语义跨平台：
        #    POSIX 用 start_new_session（新进程组/会话）；Windows 用
        #    DETACHED_PROCESS|CREATE_NEW_PROCESS_GROUP，否则子进程可能随父退出。
        try:
            args = [
                CPP_EXECUTABLE,
                "--config", config_path,  # 绝对路径，避开 QDir::setCurrent 歧义
                "--pet", profile.name,
                "--profile-id", profile.profile_id,
                "--owner-diary-bootstrap", bootstrap["path"],
            ]
            if sys.platform == "win32":
                DETACHED_PROCESS = 0x00000008
                CREATE_NEW_PROCESS_GROUP = 0x00000200
                core_process = subprocess.Popen(
                    args,
                    creationflags=DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
                    close_fds=True)
            else:
                core_process = subprocess.Popen(args, start_new_session=True)
        except OSError as e:
            self._close_owner_diary_session()
            InfoBar.error("启动失败", str(e),
                          parent=self, position=InfoBarPosition.TOP, duration=5000)
            return

        alive_count = self._pet_process_tracker.add(core_process)
        self.pet_page.set_alive_count(alive_count)

        QTimer.singleShot(
            200, lambda: self._connect_owner_diary(generation, 0))

        InfoBar.success("已启动", f"已唤起 {self.state.pet_name}（运行中：{alive_count}）",
                        parent=self, position=InfoBarPosition.TOP, duration=3000)

    def _create_owner_diary_bootstrap(self, profile_id: str) -> dict:
        token = base64.urlsafe_b64encode(
            secrets.token_bytes(32)).rstrip(b"=").decode("ascii")
        user = getpass.getuser().encode("utf-8", errors="replace")
        user_hash = hashlib.sha256(user).hexdigest()[:16]
        socket_name = (
            f"desktop-pet-owner-{user_hash}-{profile_id}-{secrets.token_hex(8)}")
        expires_at = (datetime.now(timezone.utc) + timedelta(seconds=60)) \
            .isoformat(timespec="milliseconds").replace("+00:00", "Z")
        payload = json.dumps({
            "protocolVersion": 1,
            "profileId": profile_id,
            "socketName": socket_name,
            "capabilityToken": token,
            "expiresAt": expires_at,
            "maxFrameBytes": 65536,
            "sessionTtlSeconds": 300,
        }, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        path = os.path.join(
            tempfile.gettempdir(), f"desktop-pet-owner-{secrets.token_hex(16)}.json")
        descriptor = _open_private_bootstrap(path)
        try:
            if sys.platform != "win32":
                os.chmod(path, 0o600)
            written = 0
            while written < len(payload):
                count = os.write(descriptor, payload[written:])
                if count <= 0:
                    raise OSError("failed to write owner diary bootstrap")
                written += count
            os.fsync(descriptor)
        except Exception:
            os.close(descriptor)
            self._secure_remove_bootstrap(path)
            raise
        else:
            os.close(descriptor)
        return {"path": os.path.abspath(path), "socket_name": socket_name,
                "capability_token": token}

    def _connect_owner_diary(self, generation: int, attempt: int) -> None:
        if generation != self._owner_connect_generation \
                or self._owner_socket_name is None \
                or self._owner_capability_token is None:
            return
        client = OwnerDiaryClient(timeout_ms=150)
        try:
            client.connect_to_server(
                self._owner_socket_name, self._owner_capability_token)
        except OwnerDiaryError:
            client.close()
            if attempt < 59:
                QTimer.singleShot(
                    250,
                    lambda: self._connect_owner_diary(generation, attempt + 1))
                return
            self._owner_socket_name = None
            self._owner_capability_token = None
            self._secure_remove_bootstrap(self._owner_bootstrap_path)
            self._owner_bootstrap_path = None
            self.private_diary_page.set_client(None)
            return
        self.owner_diary_client = client
        self.private_diary_page.set_client(client)
        self._owner_socket_name = None
        self._owner_capability_token = None
        self._owner_bootstrap_path = None

    @staticmethod
    def _secure_remove_bootstrap(path: str | None) -> None:
        if not path or not os.path.isfile(path):
            return
        try:
            try:
                size = os.path.getsize(path)
                with open(path, "r+b", buffering=0) as bootstrap:
                    remaining = size
                    zeros = b"\0" * 4096
                    while remaining > 0:
                        chunk = zeros[:min(remaining, len(zeros))]
                        bootstrap.write(chunk)
                        remaining -= len(chunk)
                    bootstrap.flush()
                    os.fsync(bootstrap.fileno())
            except OSError:
                pass
        finally:
            try:
                os.unlink(path)
            except OSError:
                pass

    def _close_owner_diary_session(self) -> None:
        self._owner_connect_generation += 1
        self.private_diary_page.deactivate()
        self.private_diary_page.set_client(None)
        if self.owner_diary_client is not None:
            self.owner_diary_client.close()
        self.owner_diary_client = None
        self._owner_socket_name = None
        self._owner_capability_token = None
        self._secure_remove_bootstrap(self._owner_bootstrap_path)
        self._owner_bootstrap_path = None

    def closeEvent(self, event):
        self._close_owner_diary_session()
        super().closeEvent(event)


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
