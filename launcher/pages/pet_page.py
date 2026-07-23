"""宠物设置页：角色选择 / 尺寸 / 窗口置顶 / 点击穿透。"""

from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QWidget, QVBoxLayout, QHBoxLayout
from qfluentwidgets import ScrollArea, ComboBox, Slider, SwitchButton, SpinBox

from app_state import AppState
from pet_registry import list_pet_names
from ._cards import make_card


class PetPage(ScrollArea):
    def __init__(self, state: AppState, parent=None):
        super().__init__(parent)
        self.state = state
        self.setObjectName("PetPage")

        container = QWidget()
        layout = QVBoxLayout(container)
        layout.setContentsMargins(36, 20, 36, 20)
        layout.setSpacing(12)

        # 角色选择
        self.pet_combo = ComboBox()
        self.pet_combo.setFixedWidth(220)
        self._refresh_pets()
        self.pet_combo.currentTextChanged.connect(self._on_pet_changed)
        layout.addWidget(make_card(self.pet_combo, "角色选择",
                                   "来自 pets.json 注册表，启动时经 --pet 传给 C++", "pet"))

        # 尺寸滑块 50–200 + 数字输入
        size_wrap = QWidget()
        sl = QHBoxLayout(size_wrap)
        sl.setContentsMargins(0, 0, 0, 0)
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
        sl.addSpacing(12)
        sl.addWidget(self.size_spin)
        layout.addWidget(make_card(size_wrap, "大小 / 尺寸",
                                   "对应 petSettings.scalePercent（50–200）", "pet"))

        # 窗口置顶
        top_switch = SwitchButton()
        top_switch.setChecked(state.always_on_top)
        top_switch.checkedChanged.connect(lambda c: self._on_switch("always_on_top", c))
        layout.addWidget(make_card(top_switch, "窗口置顶", "petSettings.alwaysOnTop", "pet"))

        # 点击穿透
        click_switch = SwitchButton()
        click_switch.setChecked(state.click_through)
        click_switch.checkedChanged.connect(lambda c: self._on_switch("click_through", c))
        layout.addWidget(make_card(click_switch, "点击穿透", "petSettings.clickThrough", "pet"))

        layout.addStretch(1)
        self.setWidget(container)
        self.setWidgetResizable(True)

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