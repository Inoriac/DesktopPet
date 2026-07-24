"""语音设置页：语音合成开关 / 音量 / 说话者选择。

布局对齐 demo.py（SingleDirectionScrollArea + HeaderCardWidget 分区）。
"""

from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QWidget, QHBoxLayout
from qfluentwidgets import (
    SwitchButton, Slider, ComboBox, BodyLabel,
)

from app_state import AppState
from ._ui import ScrollPage, Section


class VoicePage(ScrollPage):
    SPEAKERS = ["feibi", "mika", "thirtyseven"]

    def __init__(self, state: AppState, parent=None):
        super().__init__("VoicePage", parent)
        self.state = state

        # —— 语音 ——
        voice_card = Section("语音", self)

        sw = SwitchButton()
        sw.setChecked(state.voice_enabled)
        sw.checkedChanged.connect(lambda c: setattr(state, "voice_enabled", c))
        voice_card.addRow("语音合成开关", "voice.enabled", sw)

        # 音量 0–100% + 实时标签
        vol_wrap = QWidget()
        vl = QHBoxLayout(vol_wrap)
        vl.setContentsMargins(0, 0, 0, 0)
        vl.setSpacing(12)
        self.vol_slider = Slider(Qt.Horizontal)
        self.vol_slider.setRange(0, 100)
        self.vol_slider.setValue(state.volume_percent)
        self.vol_slider.setFixedWidth(200)
        self.vol_label = BodyLabel(f"{state.volume_percent}%")
        self.vol_label.setFixedWidth(44)
        self.vol_slider.valueChanged.connect(self._on_volume)
        vl.addWidget(self.vol_slider)
        vl.addWidget(self.vol_label)
        voice_card.addRow("音量", "appSettings.volume（0–100% ↔ 0.0–1.0）", vol_wrap)

        speaker = ComboBox()
        speaker.addItems(self.SPEAKERS)
        speaker.setCurrentText(state.speaker if state.speaker in self.SPEAKERS else "feibi")
        speaker.currentTextChanged.connect(lambda t: setattr(state, "speaker", t))
        speaker.setFixedWidth(220)
        voice_card.addRow("说话者选择", "voice.selectedSpeaker（枚举）", speaker)
        self.addCard(voice_card)

        # —— 音效（暂不实现，见计划 §十一）——
        fx_card = Section("音效", self)
        note = BodyLabel("当前项目尚未实现音效，本项暂不提供。实现后于此补字段映射。")
        fx_card.addWidget(note)
        self.addCard(fx_card)

        self.addStretch()

    def _on_volume(self, v: int):
        self.state.volume_percent = v
        self.vol_label.setText(f"{v}%")