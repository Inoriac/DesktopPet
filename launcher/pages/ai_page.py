"""AI 设置页：AI 开关 / 自动屏聊 / 聊天间隔 / API 配置(提供商/BaseURL/Key/模型/视觉模型)。"""

from __future__ import annotations

from PySide6.QtWidgets import QWidget, QVBoxLayout, QHBoxLayout
from qfluentwidgets import (
    ScrollArea, SwitchButton, SpinBox, ComboBox, LineEdit, PasswordLineEdit,
)

from app_state import AppState
from ._cards import make_card


class AiPage(ScrollArea):
    def __init__(self, state: AppState, parent=None):
        super().__init__(parent)
        self.state = state
        self.setObjectName("AiPage")

        container = QWidget()
        layout = QVBoxLayout(container)
        layout.setContentsMargins(36, 20, 36, 20)
        layout.setSpacing(12)

        # AI 开关
        sw = SwitchButton()
        sw.setChecked(state.ai_enabled)
        sw.checkedChanged.connect(lambda c: setattr(state, "ai_enabled", c))
        layout.addWidget(make_card(sw, "AI 开关", "aiSettings.profiles.default.enabled", "ai"))

        # 自动屏聊
        asc = SwitchButton()
        asc.setChecked(state.auto_screen_chat)
        asc.checkedChanged.connect(lambda c: setattr(state, "auto_screen_chat", c))
        layout.addWidget(make_card(asc, "自动屏幕聊天", "screenChat.enabled — 宠物主动发起对话", "ai"))

        # 聊天间隔 min/max
        interval_wrap = QWidget()
        il = QHBoxLayout(interval_wrap)
        il.setContentsMargins(0, 0, 0, 0)
        self.min_spin = SpinBox()
        self.min_spin.setRange(1000, 86400000)
        self.min_spin.setValue(state.chat_interval_min_ms)
        self.min_spin.setFixedWidth(140)
        self.max_spin = SpinBox()
        self.max_spin.setRange(1000, 86400000)
        self.max_spin.setValue(state.chat_interval_max_ms)
        self.max_spin.setFixedWidth(140)
        self.min_spin.valueChanged.connect(lambda v: setattr(state, "chat_interval_min_ms", v))
        self.max_spin.valueChanged.connect(lambda v: setattr(state, "chat_interval_max_ms", v))
        il.addWidget(self.min_spin)
        il.addSpacing(12)
        il.addWidget(self.max_spin)
        layout.addWidget(make_card(interval_wrap, "聊天间隔 (ms)",
                                   "screenChat.minIntervalMs / maxIntervalMs 区间", "ai"))

        # 提供商
        prov = ComboBox()
        prov.addItems(["openai-compatible"])
        prov.setCurrentText(state.provider)
        prov.currentTextChanged.connect(lambda t: setattr(state, "provider", t))
        prov.setFixedWidth(220)
        layout.addWidget(make_card(prov, "提供商", "profiles.default.provider", "ai"))

        # Base URL
        base = LineEdit()
        base.setText(state.base_url)
        base.textChanged.connect(lambda t: setattr(state, "base_url", t))
        base.setFixedWidth(360)
        layout.addWidget(make_card(base, "Base URL", "profiles.default.baseUrl", "ai"))

        # API Key
        key = PasswordLineEdit()
        key.setText(state.api_key)
        key.textChanged.connect(lambda t: setattr(state, "api_key", t))
        key.setFixedWidth(360)
        layout.addWidget(make_card(key, "API Key", "profiles.default.apiKey", "ai"))

        # 文本模型
        model = LineEdit()
        model.setText(state.model)
        model.textChanged.connect(lambda t: setattr(state, "model", t))
        model.setFixedWidth(360)
        layout.addWidget(make_card(model, "文本模型", "profiles.default.model", "ai"))

        # 视觉模型
        vmodel = LineEdit()
        vmodel.setText(state.visual_model)
        vmodel.textChanged.connect(lambda t: setattr(state, "visual_model", t))
        vmodel.setFixedWidth(360)
        layout.addWidget(make_card(vmodel, "视觉模型", "profiles.default.visual_model（下划线）", "ai"))

        layout.addStretch(1)
        self.setWidget(container)
        self.setWidgetResizable(True)