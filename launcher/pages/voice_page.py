"""语音设置页：语音合成开关 / 音量 / 说话者选择。"""

from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QWidget, QVBoxLayout, QHBoxLayout
from qfluentwidgets import (
    ScrollArea, SwitchButton, Slider, ComboBox, BodyLabel,
)

from app_state import AppState
from ._cards import make_card


class VoicePage(ScrollArea):
    SPEAKERS = ["feibi", "mika", "thirtyseven"]

    def __init__(self, state: AppState, parent=None):
        super().__init__(parent)
        self.state = state
        self.setObjectName("VoicePage")

        container = QWidget()
        layout = QVBoxLayout(container)
        layout.setContentsMargins(36, 20, 36, 20)
        layout.setSpacing(12)

        # 语音合成开关
        sw = SwitchButton()
        sw.setChecked(state.voice_enabled)
        sw.checkedChanged.connect(lambda c: setattr(state, "voice_enabled", c))
        layout.addWidget(make_card(sw, "语音合成开关", "voice.enabled", "voice"))

        # 音量 0–100%
        vol_wrap = QWidget()
        vl = QHBoxLayout(vol_wrap)
        vl.setContentsMargins(0, 0, 0, 0)
        self.vol_slider = Slider(Qt.Horizontal)
        self.vol_slider.setRange(0, 100)
        self.vol_slider.setValue(state.volume_percent)
        self.vol_slider.setFixedWidth(200)
        self.vol_label = BodyLabel(f"{state.volume_percent}%")
        self.vol_label.setFixedWidth(40)
        self.vol_slider.valueChanged.connect(self._on_volume)
        vl.addWidget(self.vol_slider)
        vl.addWidget(self.vol_label)
        layout.addWidget(make_card(vol_wrap, "音量",
                                   "appSettings.volume（0–100% ↔ 0.0–1.0）", "voice"))

        # 说话者选择
        speaker = ComboBox()
        speaker.addItems(self.SPEAKERS)
        speaker.setCurrentText(state.speaker if state.speaker in self.SPEAKERS else "feibi")
        speaker.currentTextChanged.connect(lambda t: setattr(state, "speaker", t))
        speaker.setFixedWidth(220)
        layout.addWidget(make_card(speaker, "说话者选择", "voice.selectedSpeaker（枚举）", "voice"))

        # 音效开关（暂不实现，见计划 §十一）
        note = BodyLabel("当前项目尚未实现音效，本项暂不提供。")
        layout.addWidget(make_card(note, "音效", "TODO — 实现音效后补", "voice"))

        layout.addStretch(1)
        self.setWidget(container)
        self.setWidgetResizable(True)

    def _on_volume(self, v: int):
        self.state.volume_percent = v
        self.vol_label.setText(f"{v}%")