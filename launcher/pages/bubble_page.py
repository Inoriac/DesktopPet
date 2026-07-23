"""气泡/对话泡设置页。真实 schema 中气泡参数全挂在 aiSettings.profiles.default.screenChat 下。"""

from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QWidget, QVBoxLayout, QHBoxLayout
from qfluentwidgets import ScrollArea, Slider, SpinBox, BodyLabel

from app_state import AppState
from ._cards import make_card


class BubblePage(ScrollArea):
    def __init__(self, state: AppState, parent=None):
        super().__init__(parent)
        self.state = state
        self.setObjectName("BubblePage")

        container = QWidget()
        layout = QVBoxLayout(container)
        layout.setContentsMargins(36, 20, 36, 20)
        layout.setSpacing(12)

        # 不透明度 0–100
        layout.addWidget(self._slider_card(
            "气泡不透明度", "screenChat.bubbleOpacityPercent", 0, 100,
            state.bubble_opacity, lambda v: setattr(state, "bubble_opacity", v)))

        # 字体大小
        layout.addWidget(self._spin_card(
            "气泡字体大小 (px)", "screenChat.bubbleFontSize", 1, 200,
            state.bubble_font_size, lambda v: setattr(state, "bubble_font_size", v)))

        # 水平偏移
        layout.addWidget(self._spin_card(
            "气泡水平偏移 X", "screenChat.bubbleOffsetX", -2000, 2000,
            state.bubble_offset_x, lambda v: setattr(state, "bubble_offset_x", v)))

        # 垂直偏移
        layout.addWidget(self._spin_card(
            "气泡垂直偏移 Y", "screenChat.bubbleOffsetY", -2000, 2000,
            state.bubble_offset_y, lambda v: setattr(state, "bubble_offset_y", v)))

        layout.addStretch(1)
        self.setWidget(container)
        self.setWidgetResizable(True)

    def _slider_card(self, title, content, lo, hi, value, on_change):
        wrap = QWidget()
        hl = QHBoxLayout(wrap)
        hl.setContentsMargins(0, 0, 0, 0)
        slider = Slider(Qt.Horizontal)
        slider.setRange(lo, hi)
        slider.setValue(value)
        slider.setFixedWidth(220)
        label = BodyLabel(f"{value}")
        label.setFixedWidth(48)
        slider.valueChanged.connect(lambda v: (on_change(v), label.setText(str(v))))
        hl.addWidget(slider)
        hl.addWidget(label)
        return make_card(wrap, title, content, "bubble")

    def _spin_card(self, title, content, lo, hi, value, on_change):
        wrap = QWidget()
        hl = QHBoxLayout(wrap)
        hl.setContentsMargins(0, 0, 0, 0)
        spin = SpinBox()
        spin.setRange(lo, hi)
        spin.setValue(value)
        spin.setFixedWidth(120)
        spin.valueChanged.connect(on_change)
        hl.addWidget(spin)
        return make_card(wrap, title, content, "bubble")