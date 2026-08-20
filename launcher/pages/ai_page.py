"""AI 设置页：AI 开关 / 自动屏聊 / 聊天间隔 / API 配置(提供商/BaseURL/Key/模型/视觉模型)。

布局对齐 demo.py（SingleDirectionScrollArea + HeaderCardWidget 分区）。
"""

from __future__ import annotations

from PySide6.QtWidgets import QWidget, QHBoxLayout
from qfluentwidgets import (
    SwitchButton, SpinBox, ComboBox, LineEdit, PasswordLineEdit,
)

from app_state import AppState
from ._ui import ScrollPage, Section


class AiPage(ScrollPage):
    def __init__(self, state: AppState, parent=None):
        super().__init__("AiPage", parent)
        self.state = state

        # —— 行为 ——
        beh_card = Section("行为", self)

        sw = SwitchButton()
        sw.setChecked(state.ai_enabled)
        sw.checkedChanged.connect(lambda c: setattr(state, "ai_enabled", c))
        beh_card.addRow("AI 开关", "aiSettings.profiles.default.enabled", sw)

        emotion_switch = SwitchButton()
        emotion_switch.setChecked(state.emotion_enabled)
        emotion_switch.checkedChanged.connect(
            lambda checked: setattr(state, "emotion_enabled", checked))
        beh_card.addRow("情绪系统", "emotion.enabled", emotion_switch)

        asc = SwitchButton()
        asc.setChecked(state.auto_screen_chat)
        asc.checkedChanged.connect(lambda c: setattr(state, "auto_screen_chat", c))
        beh_card.addRow("自动屏幕聊天", "screenChat.enabled — 宠物主动发起对话", asc)

        interval_wrap = QWidget()
        il = QHBoxLayout(interval_wrap)
        il.setContentsMargins(0, 0, 0, 0)
        il.setSpacing(12)
        from PySide6.QtGui import QFont
        self.min_spin = SpinBox()
        self.min_spin.setRange(1000, 86400000)
        self.min_spin.setValue(state.chat_interval_min_ms)
        self.min_spin.setFixedWidth(160)
        _sf = QFont(self.min_spin.font().family()); _sf.setPointSize(11)
        self.min_spin.setFont(_sf)
        self.max_spin = SpinBox()
        self.max_spin.setRange(1000, 86400000)
        self.max_spin.setValue(state.chat_interval_max_ms)
        self.max_spin.setFixedWidth(160)
        self.max_spin.setFont(_sf)
        self.min_spin.valueChanged.connect(lambda v: setattr(state, "chat_interval_min_ms", v))
        self.max_spin.valueChanged.connect(lambda v: setattr(state, "chat_interval_max_ms", v))
        il.addWidget(self.min_spin)
        il.addWidget(self.max_spin)
        beh_card.addRow("聊天间隔 (ms)",
                        "screenChat.minIntervalMs / maxIntervalMs 区间", interval_wrap)
        self.addCard(beh_card)

        # —— API 配置 ——
        api_card = Section("API 配置", self)

        prov = ComboBox()
        prov.addItems(["openai-compatible"])
        prov.setCurrentText(state.provider)
        prov.currentTextChanged.connect(lambda t: setattr(state, "provider", t))
        prov.setFixedWidth(220)
        api_card.addRow("提供商", "profiles.default.provider", prov)

        base = LineEdit()
        base.setText(state.base_url)
        base.textChanged.connect(lambda t: setattr(state, "base_url", t))
        base.setFixedWidth(360)
        api_card.addRow("Base URL", "profiles.default.baseUrl", base)

        key = PasswordLineEdit()
        key.setText(state.api_key)
        key.textChanged.connect(lambda t: setattr(state, "api_key", t))
        key.setFixedWidth(360)
        api_card.addRow("API Key", "profiles.default.apiKey", key)

        model = LineEdit()
        model.setText(state.model)
        model.textChanged.connect(lambda t: setattr(state, "model", t))
        model.setFixedWidth(360)
        api_card.addRow("文本模型", "profiles.default.model", model)

        vmodel = LineEdit()
        vmodel.setText(state.visual_model)
        vmodel.textChanged.connect(lambda t: setattr(state, "visual_model", t))
        vmodel.setFixedWidth(360)
        api_card.addRow("视觉模型", "profiles.default.visual_model（下划线）", vmodel)
        self.addCard(api_card)

        self.addStretch()
