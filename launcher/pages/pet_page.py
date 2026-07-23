"""宠物设置页：角色选择 / 尺寸 / 窗口置顶 / 点击穿透。

布局对齐 demo.py（SingleDirectionScrollArea + HeaderCardWidget 分区）。
"""

from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QWidget, QHBoxLayout
from qfluentwidgets import ComboBox, Slider, SpinBox, SwitchButton, PrimaryPushButton
from qfluentwidgets import FluentIcon as FIF

from app_state import AppState
from pet_registry import list_pet_names
from ._ui import ScrollPage, Section


class PetPage(ScrollPage):
    def __init__(self, state: AppState, on_start=None, parent=None):
        super().__init__("PetPage", parent)
        self.state = state
        self._on_start = on_start

        # —— 角色与外观 ——
        char_card = Section("角色与外观", self)

        self.pet_combo = ComboBox()
        self.pet_combo.setFixedWidth(220)
        self._refresh_pets()
        self.pet_combo.currentTextChanged.connect(self._on_pet_changed)
        char_card.addRow("角色选择",
                         "来自 pets.json 注册表，启动时经 --pet 传给 C++",
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
        self.size_spin = SpinBox()
        self.size_spin.setRange(50, 200)
        self.size_spin.setValue(state.scale_percent)
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
        self.start_btn = PrimaryPushButton(FIF.PLAY, "启动桌宠", start_row)
        self.start_btn.setFixedHeight(44)
        self.start_btn.setMinimumWidth(180)
        if self._on_start is not None:
            self.start_btn.clicked.connect(self._on_start)
        rl.addStretch(1)
        rl.addWidget(self.start_btn, 0, Qt.AlignRight | Qt.AlignVCenter)
        self.addCard(start_row)

        self.addStretch()

    def _refresh_pets(self):
        names = list_pet_names() or ["Milltina"]
        self.pet_combo.clear()
        self.pet_combo.addItems(names)
        if self.state.pet_name in names:
            self.pet_combo.setCurrentText(self.state.pet_name)
        else:
            self.state.pet_name = names[0]
            self.pet_combo.setCurrentText(names[0])

    def _on_pet_changed(self, name: str):
        self.state.pet_name = name

    def _on_size_changed(self, value: int):
        self.state.scale_percent = value
        if self.size_slider.value() != value:
            self.size_slider.setValue(value)
        if self.size_spin.value() != value:
            self.size_spin.setValue(value)

    def _on_switch(self, attr: str, checked: bool):
        setattr(self.state, attr, checked)