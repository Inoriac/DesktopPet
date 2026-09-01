"""高级设置页：渲染(抗锯齿/阴影/纹理) / 交互(拖拽阈值/点击超时) / 窗口吸附(开关/阈值/垂直偏移)。

主题切换不放此页——由 main.py 经 QSettings 与 C++ 共享（计划 §3.1）。
布局对齐 demo.py（SingleDirectionScrollArea + HeaderCardWidget 分区）。
"""

from __future__ import annotations

from PySide6.QtGui import QFont
from PySide6.QtWidgets import QWidget, QHBoxLayout
from qfluentwidgets import ComboBox, CompactSpinBox, SwitchButton

from app_state import AppState
from ._ui import ScrollPage, Section


class AdvancedPage(ScrollPage):
    QUALITIES = ["low", "medium", "high"]

    def __init__(self, state: AppState, parent=None):
        super().__init__("AdvancedPage", parent)
        self.state = state

        # —— 渲染 ——
        render_card = Section("渲染", self)

        aa = SwitchButton()
        aa.setChecked(state.antialiasing)
        aa.checkedChanged.connect(lambda c: setattr(state, "antialiasing", c))
        render_card.addRow("抗锯齿 (MSAA)", "renderSettings.antialiasing", aa)

        shadow = ComboBox()
        shadow.addItems(self.QUALITIES)
        shadow.setCurrentText(state.shadow_quality)
        shadow.currentTextChanged.connect(lambda t: setattr(state, "shadow_quality", t))
        shadow.setFixedWidth(160)
        render_card.addRow("阴影质量", "renderSettings.shadowQuality", shadow)

        texture = ComboBox()
        texture.addItems(self.QUALITIES)
        texture.setCurrentText(state.texture_quality)
        texture.currentTextChanged.connect(lambda t: setattr(state, "texture_quality", t))
        texture.setFixedWidth(160)
        render_card.addRow("纹理质量", "renderSettings.textureQuality", texture)
        self.addCard(render_card)

        # —— 交互 ——
        inter_card = Section("交互", self)
        inter_card.addRow("拖拽阈值 (像素)", "interactionSettings.dragThreshold",
                          self._spin(1, 200, state.drag_threshold,
                                     lambda v: setattr(state, "drag_threshold", v)))
        inter_card.addRow("点击超时 (ms)", "interactionSettings.clickTimeout",
                          self._spin(10, 10000, state.click_timeout,
                                     lambda v: setattr(state, "click_timeout", v)))
        self.addCard(inter_card)

        # —— 窗口吸附 ——
        snap_card = Section("窗口吸附", self)

        snap = SwitchButton()
        snap.setChecked(state.snap_enabled)
        snap.checkedChanged.connect(lambda c: setattr(state, "snap_enabled", c))
        snap_card.addRow("窗口吸附开关", "windowSnapping.enabled", snap)

        snap_card.addRow("吸附阈值 (像素)", "windowSnapping.snapThreshold",
                         self._spin(1, 500, state.snap_threshold,
                                    lambda v: setattr(state, "snap_threshold", v)))
        snap_card.addRow("垂直偏移 (像素)", "windowSnapping.verticalOffset",
                         self._spin(-2000, 2000, state.snap_vertical_offset,
                                    lambda v: setattr(state, "snap_vertical_offset", v)))
        self.addCard(snap_card)

        self.addStretch()

    @staticmethod
    def _spin(lo, hi, value, on_change) -> QWidget:
        wrap = QWidget()
        hl = QHBoxLayout(wrap)
        hl.setContentsMargins(0, 0, 0, 0)
        spin = CompactSpinBox()
        spin.setRange(lo, hi)
        spin.setValue(value)
        spin.setFixedWidth(132)
        f = QFont(spin.font().family())
        f.setPointSize(11)
        spin.setFont(f)
        spin.valueChanged.connect(on_change)
        hl.addWidget(spin)
        return wrap
