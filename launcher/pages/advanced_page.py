"""高级设置页：渲染(抗锯齿/阴影/纹理) / 交互(拖拽阈值/点击超时) / 窗口吸附(开关/阈值/垂直偏移)。

主题切换不放此页——由 main.py 经 QSettings 与 C++ 共享（计划 §3.1）。
"""

from __future__ import annotations

from PySide6.QtWidgets import QWidget, QVBoxLayout, QHBoxLayout
from qfluentwidgets import (
    ScrollArea, SwitchButton, ComboBox, SpinBox,
)

from app_state import AppState
from ._cards import make_card


class AdvancedPage(ScrollArea):
    QUALITIES = ["low", "medium", "high"]

    def __init__(self, state: AppState, parent=None):
        super().__init__(parent)
        self.state = state
        self.setObjectName("AdvancedPage")

        container = QWidget()
        layout = QVBoxLayout(container)
        layout.setContentsMargins(36, 20, 36, 20)
        layout.setSpacing(12)

        # —— 渲染 ——
        aa = SwitchButton()
        aa.setChecked(state.antialiasing)
        aa.checkedChanged.connect(lambda c: setattr(state, "antialiasing", c))
        layout.addWidget(make_card(aa, "抗锯齿 (MSAA)", "renderSettings.antialiasing", "gear"))

        shadow = ComboBox()
        shadow.addItems(self.QUALITIES)
        shadow.setCurrentText(state.shadow_quality)
        shadow.currentTextChanged.connect(lambda t: setattr(state, "shadow_quality", t))
        shadow.setFixedWidth(160)
        layout.addWidget(make_card(shadow, "阴影质量", "renderSettings.shadowQuality", "gear"))

        texture = ComboBox()
        texture.addItems(self.QUALITIES)
        texture.setCurrentText(state.texture_quality)
        texture.currentTextChanged.connect(lambda t: setattr(state, "texture_quality", t))
        texture.setFixedWidth(160)
        layout.addWidget(make_card(texture, "纹理质量", "renderSettings.textureQuality", "gear"))

        # —— 交互 ——
        layout.addWidget(self._spin_card("拖拽阈值 (像素)", 1, 200,
            state.drag_threshold, lambda v: setattr(state, "drag_threshold", v),
            "interactionSettings.dragThreshold"))
        layout.addWidget(self._spin_card("点击超时 (ms)", 10, 10000,
            state.click_timeout, lambda v: setattr(state, "click_timeout", v),
            "interactionSettings.clickTimeout"))

        # —— 窗口吸附 ——
        snap = SwitchButton()
        snap.setChecked(state.snap_enabled)
        snap.checkedChanged.connect(lambda c: setattr(state, "snap_enabled", c))
        layout.addWidget(make_card(snap, "窗口吸附开关", "windowSnapping.enabled", "gear"))

        layout.addWidget(self._spin_card("吸附阈值 (像素)", 1, 500,
            state.snap_threshold, lambda v: setattr(state, "snap_threshold", v),
            "windowSnapping.snapThreshold"))
        layout.addWidget(self._spin_card("垂直偏移 (像素)", -2000, 2000,
            state.snap_vertical_offset, lambda v: setattr(state, "snap_vertical_offset", v),
            "windowSnapping.verticalOffset"))

        layout.addStretch(1)
        self.setWidget(container)
        self.setWidgetResizable(True)

    def _spin_card(self, title, lo, hi, value, on_change, content):
        wrap = QWidget()
        hl = QHBoxLayout(wrap)
        hl.setContentsMargins(0, 0, 0, 0)
        spin = SpinBox()
        spin.setRange(lo, hi)
        spin.setValue(value)
        spin.setFixedWidth(120)
        spin.valueChanged.connect(on_change)
        hl.addWidget(spin)
        return make_card(wrap, title, content, "gear")