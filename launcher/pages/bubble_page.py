"""气泡/对话泡设置页。真实 schema 中气泡参数全挂在 aiSettings.profiles.default.screenChat 下。

布局对齐 demo.py（SingleDirectionScrollArea + HeaderCardWidget 分区）。
"""

from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtGui import QFont
from PySide6.QtWidgets import QWidget, QHBoxLayout
from qfluentwidgets import BodyLabel, CompactSpinBox, Slider

from app_state import AppState
from ._ui import ScrollPage, Section


class BubblePage(ScrollPage):
    def __init__(self, state: AppState, parent=None):
        super().__init__("BubblePage", parent)
        self.state = state

        card = Section("对话气泡", self)

        # 不透明度 0–100
        card.addRow("气泡不透明度", "screenChat.bubbleOpacityPercent",
                    self._slider(0, 100, state.bubble_opacity,
                                 lambda v: setattr(state, "bubble_opacity", v)))

        # 字体大小
        card.addRow("气泡字体大小 (px)", "screenChat.bubbleFontSize",
                    self._spin(1, 200, state.bubble_font_size,
                               lambda v: setattr(state, "bubble_font_size", v)))

        # 水平偏移
        card.addRow("气泡水平偏移 X", "screenChat.bubbleOffsetX",
                    self._spin(-2000, 2000, state.bubble_offset_x,
                               lambda v: setattr(state, "bubble_offset_x", v)))

        # 垂直偏移
        card.addRow("气泡垂直偏移 Y", "screenChat.bubbleOffsetY",
                    self._spin(-2000, 2000, state.bubble_offset_y,
                               lambda v: setattr(state, "bubble_offset_y", v)))

        self.addCard(card)
        self.addStretch()

    @staticmethod
    def _slider(lo, hi, value, on_change) -> QWidget:
        wrap = QWidget()
        hl = QHBoxLayout(wrap)
        hl.setContentsMargins(0, 0, 0, 0)
        hl.setSpacing(12)
        slider = Slider(Qt.Horizontal)
        slider.setRange(lo, hi)
        slider.setValue(value)
        slider.setFixedWidth(220)
        label = BodyLabel(str(value))
        label.setFixedWidth(48)
        slider.valueChanged.connect(lambda v: (on_change(v), label.setText(str(v))))
        hl.addWidget(slider)
        hl.addWidget(label)
        return wrap

    @staticmethod
    def _spin(lo, hi, value, on_change) -> CompactSpinBox:
        spin = CompactSpinBox()
        spin.setRange(lo, hi)
        spin.setValue(value)
        spin.setFixedWidth(132)
        # 保证负数和四位数在右侧紧凑调整区旁完整显示。
        f = QFont(spin.font().family())
        f.setPointSize(11)
        spin.setFont(f)
        spin.valueChanged.connect(on_change)
        return spin
