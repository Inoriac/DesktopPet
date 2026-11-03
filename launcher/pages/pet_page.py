"""宠物设置页：角色选择 / 尺寸 / 窗口置顶 / 点击穿透。

布局对齐 demo.py（SingleDirectionScrollArea + HeaderCardWidget 分区）。
"""

from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QWidget, QHBoxLayout
from qfluentwidgets import (
    BodyLabel, ComboBox, CompactSpinBox, PrimaryPushButton, PushButton, Slider,
    SwitchButton,
)
from qfluentwidgets import FluentIcon as FIF

from app_state import AppState
from pet_registry import PetRegistryError, load_pets
from ._ui import ScrollPage, Section


class PetPage(ScrollPage):
    def __init__(self, state: AppState, on_start=None,
                 on_toggle_avatar=None, on_stop=None, parent=None):
        super().__init__("PetPage", parent)
        self.state = state
        self._on_start = on_start
        self._on_toggle_avatar = on_toggle_avatar
        self._on_stop = on_stop
        self._avatar_visible = True
        self._chat_connected = False
        self._alive_count = 0
        self.registry_error: str | None = None

        # —— 角色与外观 ——
        char_card = Section("角色与外观", self)

        self.pet_combo = ComboBox()
        self.pet_combo.setFixedWidth(220)
        self._refresh_pets()
        self.pet_combo.currentTextChanged.connect(self._on_pet_changed)
        char_card.addRow("角色选择",
                         "来自 pets.json 角色清单",
                         self.pet_combo)

        # 尺寸：滑块 + 数字输入
        size_wrap = QWidget()
        sl = QHBoxLayout(size_wrap)
        sl.setContentsMargins(0, 0, 0, 0)
        sl.setSpacing(12)
        self.size_slider = Slider(Qt.Horizontal)
        self.size_slider.setRange(50, 200)
        self.size_slider.setValue(state.scale_percent)
        self.size_slider.setFixedWidth(180)
        self.size_slider.valueChanged.connect(self._on_size_changed)
        self.size_spin = CompactSpinBox()
        self.size_spin.setRange(50, 200)
        self.size_spin.setValue(state.scale_percent)
        self.size_spin.setFixedWidth(132)
        self.size_spin.valueChanged.connect(self._on_size_changed)
        sl.addWidget(self.size_slider)
        sl.addWidget(self.size_spin)
        char_card.addRow("大小 / 尺寸", "petSettings.scalePercent（50–200）", size_wrap)
        self.addCard(char_card)

        # —— 窗口行为 ——
        win_card = Section("窗口行为", self)

        top_switch = SwitchButton()
        top_switch.setChecked(state.always_on_top)
        top_switch.checkedChanged.connect(lambda c: self._on_switch("always_on_top", c))
        win_card.addRow("窗口置顶", "petSettings.alwaysOnTop", top_switch)

        click_switch = SwitchButton()
        click_switch.setChecked(state.click_through)
        click_switch.checkedChanged.connect(lambda c: self._on_switch("click_through", c))
        win_card.addRow("点击穿透", "petSettings.clickThrough", click_switch)
        self.addCard(win_card)

        # —— 启动桌宠主按钮（首页右下角）——
        start_row = QWidget()
        rl = QHBoxLayout(start_row)
        rl.setContentsMargins(0, 8, 0, 0)
        rl.setSpacing(12)
        self.alive_count_label = BodyLabel("运行中：0", start_row)
        self.alive_count_label.setMinimumWidth(88)
        self.toggle_avatar_btn = PushButton("隐藏形象", start_row)
        self.toggle_avatar_btn.setFixedHeight(44)
        self.toggle_avatar_btn.setMinimumWidth(116)
        self.toggle_avatar_btn.setEnabled(False)
        if self._on_toggle_avatar is not None:
            self.toggle_avatar_btn.clicked.connect(self._on_toggle_avatar)
        self.stop_btn = PushButton("关闭桌宠", start_row)
        self.stop_btn.setFixedHeight(44)
        self.stop_btn.setMinimumWidth(116)
        self.stop_btn.setEnabled(False)
        if self._on_stop is not None:
            self.stop_btn.clicked.connect(self._on_stop)
        self.start_btn = PrimaryPushButton(FIF.PLAY, "启动桌宠", start_row)
        self.start_btn.setFixedHeight(44)
        self.start_btn.setMinimumWidth(180)
        self.start_btn.setEnabled(bool(self._profiles_by_name))
        if self._on_start is not None:
            self.start_btn.clicked.connect(self._request_start)
        rl.addStretch(1)
        rl.addWidget(self.alive_count_label, 0, Qt.AlignRight | Qt.AlignVCenter)
        rl.addWidget(self.toggle_avatar_btn, 0, Qt.AlignRight | Qt.AlignVCenter)
        rl.addWidget(self.stop_btn, 0, Qt.AlignRight | Qt.AlignVCenter)
        rl.addWidget(self.start_btn, 0, Qt.AlignRight | Qt.AlignVCenter)
        self.addCard(start_row)

        self.addStretch()

    def _refresh_pets(self):
        try:
            profiles = load_pets()
        except (PetRegistryError, OSError) as error:
            self.registry_error = str(error)
            profiles = []
        self._profiles_by_name = {profile.name: profile for profile in profiles}
        names = list(self._profiles_by_name)
        self.pet_combo.clear()
        self.pet_combo.addItems(names)
        if not names:
            self.state.pet_name = ""
            self.state.pet_profile_id = ""
            return
        if self.state.pet_name in names:
            self.pet_combo.setCurrentText(self.state.pet_name)
        else:
            self.state.pet_name = names[0]
            self.pet_combo.setCurrentText(names[0])
        self.state.pet_profile_id = self._profiles_by_name[self.state.pet_name].profile_id

    def set_alive_count(self, count: int) -> None:
        self._alive_count = max(0, int(count))
        self.alive_count_label.setText(f"运行中：{self._alive_count}")
        self._sync_runtime_controls()

    def set_chat_connected(self, connected: bool) -> None:
        self._chat_connected = bool(connected)
        if not self._chat_connected:
            self._avatar_visible = True
        self._sync_runtime_controls()

    def set_avatar_visible(self, visible: bool) -> None:
        self._avatar_visible = bool(visible)
        self._sync_runtime_controls()

    def _sync_runtime_controls(self) -> None:
        self.toggle_avatar_btn.setText(
            "隐藏形象" if self._avatar_visible else "显示形象")
        self.toggle_avatar_btn.setEnabled(self._chat_connected)
        self.stop_btn.setEnabled(self._alive_count > 0 or self._chat_connected)

    def set_starting(self, starting: bool) -> None:
        self.start_btn.setText("启动中" if starting else "启动桌宠")
        self.start_btn.setEnabled(
            bool(self._profiles_by_name) and not starting)

    def _request_start(self) -> None:
        self.set_starting(True)
        try:
            started = bool(self._on_start and self._on_start())
        except Exception:
            self.set_starting(False)
            raise
        if not started:
            self.set_starting(False)

    def _on_pet_changed(self, name: str):
        self.state.pet_name = name
        profile = self._profiles_by_name.get(name)
        self.state.pet_profile_id = profile.profile_id if profile else ""

    def _on_size_changed(self, value: int):
        self.state.scale_percent = value
        if self.size_slider.value() != value:
            self.size_slider.setValue(value)
        if self.size_spin.value() != value:
            self.size_spin.setValue(value)

    def _on_switch(self, attr: str, checked: bool):
        setattr(self.state, attr, checked)
