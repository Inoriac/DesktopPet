"""AI behavior, connection profiles, and model role settings."""

from __future__ import annotations

import json
import re
import uuid

from PySide6.QtGui import QFont
from PySide6.QtWidgets import QWidget, QHBoxLayout
from qfluentwidgets import (
    ComboBox,
    FluentIcon as FIF,
    InfoBar,
    InfoBarPosition,
    LineEdit,
    PasswordLineEdit,
    PrimaryPushButton,
    PushButton,
    SpinBox,
    SwitchButton,
)

from api_connection_tester import ApiConnectionTester
from app_state import AppState, ModelEndpointState, MODEL_ROLES
from ._ui import ScrollPage, Section


ENDPOINT_ID_PATTERN = re.compile(r"^[A-Za-z][A-Za-z0-9_-]{0,63}$")
ROLE_LABELS = {
    "dialogue": ("对话", "桌宠主要文字对话"),
    "vision": ("视觉", "屏幕图像识别"),
    "fastExtract": ("快速提取", "轻量结构提取"),
    "consolidation": ("记忆整理", "记忆合并与归档"),
    "diary": ("日记生成", "私人日记内容生成"),
    "daydream": ("Daydream", "可选择独立的轻量模型"),
}


class AiPage(ScrollPage):
    def __init__(self, state: AppState, parent=None, on_save=None):
        super().__init__("AiPage", parent)
        self.state = state
        self.connection_tester = ApiConnectionTester(self)
        self._active_connection_test_ids: dict[str, str | None] = {
            endpoint_id: None for endpoint_id in state.model_endpoints}
        self._connection_test_buttons = {}
        self._role_endpoint_combos = {}
        self._test_models_by_endpoint = {}
        self.connection_tester.finished.connect(
            self._on_connection_test_finished)

        self._build_behavior_section()
        self._build_endpoint_section()
        self._build_role_section(on_save)
        self.addStretch()

    def _build_behavior_section(self) -> None:
        section = Section("行为", self)
        ai_switch = SwitchButton()
        ai_switch.setChecked(self.state.ai_enabled)
        ai_switch.checkedChanged.connect(
            lambda checked: setattr(self.state, "ai_enabled", checked))
        section.addRow("AI 开关", "启用模型对话", ai_switch)

        emotion_switch = SwitchButton()
        emotion_switch.setChecked(self.state.emotion_enabled)
        emotion_switch.checkedChanged.connect(
            lambda checked: setattr(self.state, "emotion_enabled", checked))
        section.addRow("情绪系统", "启用情绪状态系统", emotion_switch)

        screen_chat_switch = SwitchButton()
        screen_chat_switch.setChecked(self.state.auto_screen_chat)
        screen_chat_switch.checkedChanged.connect(
            lambda checked: setattr(self.state, "auto_screen_chat", checked))
        section.addRow(
            "自动屏幕聊天", "允许桌宠定时识别屏幕并主动对话",
            screen_chat_switch)

        interval_wrap = QWidget()
        interval_layout = QHBoxLayout(interval_wrap)
        interval_layout.setContentsMargins(0, 0, 0, 0)
        interval_layout.setSpacing(12)
        self.min_spin = SpinBox()
        self.min_spin.setRange(1000, 86400000)
        self.min_spin.setValue(self.state.chat_interval_min_ms)
        self.min_spin.setFixedWidth(160)
        spin_font = QFont(self.min_spin.font().family())
        spin_font.setPointSize(11)
        self.min_spin.setFont(spin_font)
        self.max_spin = SpinBox()
        self.max_spin.setRange(1000, 86400000)
        self.max_spin.setValue(self.state.chat_interval_max_ms)
        self.max_spin.setFixedWidth(160)
        self.max_spin.setFont(spin_font)
        self.min_spin.valueChanged.connect(
            lambda value: setattr(
                self.state, "chat_interval_min_ms", value))
        self.max_spin.valueChanged.connect(
            lambda value: setattr(
                self.state, "chat_interval_max_ms", value))
        interval_layout.addWidget(self.min_spin)
        interval_layout.addWidget(self.max_spin)
        section.addRow(
            "聊天间隔 (ms)", "主动聊天的最小和最大间隔", interval_wrap)
        self.addCard(section)

    def _build_endpoint_section(self) -> None:
        section = Section("连接档案", self)
        selector_wrap = QWidget(section)
        selector_layout = QHBoxLayout(selector_wrap)
        selector_layout.setContentsMargins(0, 0, 0, 0)
        selector_layout.setSpacing(8)
        self.endpoint_selector = ComboBox(selector_wrap)
        self.endpoint_selector.setMinimumWidth(220)
        self.endpoint_selector.addItems(list(self.state.model_endpoints))
        self.endpoint_selector.currentTextChanged.connect(
            self._select_endpoint)
        selector_layout.addWidget(self.endpoint_selector)
        self.delete_endpoint_button = PushButton(FIF.DELETE, "", selector_wrap)
        self.delete_endpoint_button.setFixedSize(36, 36)
        self.delete_endpoint_button.setToolTip("删除连接档案")
        self.delete_endpoint_button.clicked.connect(self._delete_current_endpoint)
        selector_layout.addWidget(self.delete_endpoint_button)
        section.addRow("当前档案", "DEFAULT 可编辑但不可删除", selector_wrap)

        add_wrap = QWidget(section)
        add_layout = QHBoxLayout(add_wrap)
        add_layout.setContentsMargins(0, 0, 0, 0)
        add_layout.setSpacing(8)
        self.new_endpoint_id = LineEdit(add_wrap)
        self.new_endpoint_id.setPlaceholderText("VISION_VENDOR")
        self.new_endpoint_id.setMinimumWidth(220)
        add_layout.addWidget(self.new_endpoint_id)
        add_button = PushButton(FIF.ADD, "", add_wrap)
        add_button.setFixedSize(36, 36)
        add_button.setToolTip("新增连接档案")
        add_button.clicked.connect(self._add_endpoint)
        add_layout.addWidget(add_button)
        section.addRow("新增档案", "字母开头，可使用数字、_ 和 -", add_wrap)

        self.endpoint_provider = ComboBox(section)
        self.endpoint_provider.addItems(
            ["openai-compatible", "anthropic-messages"])
        self.endpoint_provider.setFixedWidth(220)
        self.endpoint_provider.currentTextChanged.connect(
            lambda value: self._set_endpoint_value("provider", value))
        section.addRow("提供商", "连接协议", self.endpoint_provider)

        self.endpoint_base_url = self._endpoint_line_edit(
            section, "base_url")
        section.addRow("Base URL", "模型服务地址", self.endpoint_base_url)

        self.endpoint_api_key = PasswordLineEdit(section)
        self._size_text_control(self.endpoint_api_key)
        self.endpoint_api_key.textChanged.connect(
            lambda value: self._set_endpoint_value("api_key", value))
        section.addRow(
            "API Key", "只保存于当前连接档案", self.endpoint_api_key)

        self.endpoint_anthropic_version = self._endpoint_line_edit(
            section, "anthropic_version")
        self.anthropic_version_row = section.addRow(
            "Anthropic Version", "请求头协议版本",
            self.endpoint_anthropic_version)

        self.endpoint_extra_headers = LineEdit(section)
        self._size_text_control(self.endpoint_extra_headers)
        self.endpoint_extra_headers.setPlaceholderText(
            '{"x-gateway-tenant":"desktop-pet"}')
        self.endpoint_extra_headers.textChanged.connect(
            self._set_extra_headers)
        section.addRow(
            "额外请求头", "JSON 字符串键值对象", self.endpoint_extra_headers)

        self.test_model = LineEdit(section)
        self._size_text_control(self.test_model)
        self.test_model.textChanged.connect(self._remember_test_model)
        section.addRow("测试模型", "仅用于当前连接测试", self.test_model)

        self.test_connection_button = PushButton(
            FIF.SYNC, "测试连接", section)
        self.test_connection_button.clicked.connect(
            self._test_current_endpoint)
        section.addRow(
            "连接测试", "使用当前未保存的编辑值", self.test_connection_button)
        self.addCard(section)

        if not self.state.model_endpoints:
            self.state.model_endpoints["DEFAULT"] = ModelEndpointState()
            self.endpoint_selector.addItem("DEFAULT")
        self.endpoint_selector.setCurrentText("DEFAULT")
        self._select_endpoint(self.endpoint_selector.currentText())

    def _build_role_section(self, on_save) -> None:
        section = Section("模型分工", self)
        endpoint_ids = list(self.state.model_endpoints)
        for role in MODEL_ROLES:
            role_state = self.state.model_roles[role]
            wrap = QWidget(section)
            layout = QHBoxLayout(wrap)
            layout.setContentsMargins(0, 0, 0, 0)
            layout.setSpacing(8)
            endpoint_combo = ComboBox(wrap)
            endpoint_combo.addItems(endpoint_ids)
            if role_state.endpoint_ref not in endpoint_ids:
                endpoint_combo.addItem(role_state.endpoint_ref)
            endpoint_combo.setCurrentText(role_state.endpoint_ref)
            endpoint_combo.setFixedWidth(180)
            endpoint_combo.currentTextChanged.connect(
                lambda value, active_role=role:
                setattr(self.state.model_roles[active_role],
                        "endpoint_ref", value))
            model = LineEdit(wrap)
            model.setText(role_state.model)
            model.setMinimumWidth(200)
            model.setMaximumWidth(300)
            model.textChanged.connect(
                lambda value, active_role=role:
                setattr(self.state.model_roles[active_role], "model", value))
            layout.addWidget(endpoint_combo)
            layout.addWidget(model)
            self._role_endpoint_combos[role] = endpoint_combo
            label, description = ROLE_LABELS[role]
            section.addRow(label, description, wrap)

        save_row = QWidget(section)
        save_layout = QHBoxLayout(save_row)
        save_layout.setContentsMargins(0, 4, 0, 0)
        save_layout.addStretch(1)
        self.save_btn = PrimaryPushButton(FIF.SAVE, "保存配置", save_row)
        self.save_btn.setFixedHeight(40)
        self.save_btn.setMinimumWidth(150)
        if on_save is not None:
            self.save_btn.clicked.connect(on_save)
        save_layout.addWidget(self.save_btn)
        section.addWidget(save_row)
        self.addCard(section)

    @staticmethod
    def _size_text_control(control: QWidget) -> None:
        control.setMinimumWidth(260)
        control.setMaximumWidth(360)

    def _endpoint_line_edit(self, parent: QWidget, field_name: str) -> LineEdit:
        control = LineEdit(parent)
        self._size_text_control(control)
        control.textChanged.connect(
            lambda value: self._set_endpoint_value(field_name, value))
        return control

    def _current_endpoint_id(self) -> str:
        return self.endpoint_selector.currentText()

    def _current_endpoint(self) -> ModelEndpointState | None:
        return self.state.model_endpoints.get(self._current_endpoint_id())

    def _set_endpoint_value(self, field_name: str, value: str) -> None:
        endpoint = self._current_endpoint()
        if endpoint is None:
            return
        setattr(endpoint, field_name, value)
        if field_name == "provider":
            self.anthropic_version_row.setVisible(
                value == "anthropic-messages")

    def _set_extra_headers(self, value: str) -> None:
        endpoint = self._current_endpoint()
        if endpoint is None:
            return
        if not value.strip():
            endpoint.extra_headers = {}
            return
        try:
            parsed = json.loads(value)
        except json.JSONDecodeError:
            return
        if (isinstance(parsed, dict) and
                all(isinstance(key, str) and isinstance(item, str)
                    for key, item in parsed.items())):
            endpoint.extra_headers = parsed

    def _select_endpoint(self, endpoint_id: str) -> None:
        endpoint = self.state.model_endpoints.get(endpoint_id)
        if endpoint is None:
            return
        controls = (
            self.endpoint_provider,
            self.endpoint_base_url,
            self.endpoint_api_key,
            self.endpoint_anthropic_version,
            self.endpoint_extra_headers,
            self.test_model,
        )
        for control in controls:
            control.blockSignals(True)
        self.endpoint_provider.setCurrentText(endpoint.provider)
        self.endpoint_base_url.setText(endpoint.base_url)
        self.endpoint_api_key.setText(endpoint.api_key)
        self.endpoint_anthropic_version.setText(endpoint.anthropic_version)
        self.endpoint_extra_headers.setText(json.dumps(
            endpoint.extra_headers, ensure_ascii=False, separators=(",", ":")))
        model = self._test_models_by_endpoint.get(endpoint_id)
        if model is None:
            model = next((
                role.model for role in self.state.model_roles.values()
                if role.endpoint_ref == endpoint_id and role.model), "")
        self.test_model.setText(model)
        for control in controls:
            control.blockSignals(False)
        self.anthropic_version_row.setVisible(
            endpoint.provider == "anthropic-messages")
        self.delete_endpoint_button.setEnabled(endpoint_id != "DEFAULT")
        self._connection_test_buttons[endpoint_id] = self.test_connection_button
        self.test_connection_button.setEnabled(
            not bool(self._active_connection_test_ids.get(endpoint_id)))

    def _remember_test_model(self, model: str) -> None:
        endpoint_id = self._current_endpoint_id()
        if endpoint_id:
            self._test_models_by_endpoint[endpoint_id] = model

    def _add_endpoint(self) -> None:
        endpoint_id = self.new_endpoint_id.text().strip()
        if (not ENDPOINT_ID_PATTERN.fullmatch(endpoint_id) or
                endpoint_id in self.state.model_endpoints):
            InfoBar.error(
                "无法新增连接档案", "档案 ID 无效或已存在", parent=self,
                position=InfoBarPosition.TOP, duration=4000)
            return
        self.state.model_endpoints[endpoint_id] = ModelEndpointState()
        self._active_connection_test_ids[endpoint_id] = None
        self.endpoint_selector.addItem(endpoint_id)
        for combo in self._role_endpoint_combos.values():
            combo.addItem(endpoint_id)
        self.new_endpoint_id.clear()
        self.endpoint_selector.setCurrentText(endpoint_id)

    def _delete_current_endpoint(self) -> None:
        endpoint_id = self._current_endpoint_id()
        if endpoint_id == "DEFAULT":
            return
        if any(role.endpoint_ref == endpoint_id
               for role in self.state.model_roles.values()):
            InfoBar.error(
                "无法删除连接档案", "请先将引用它的角色切换到其他档案",
                parent=self, position=InfoBarPosition.TOP, duration=4000)
            return
        self.state.model_endpoints.pop(endpoint_id, None)
        self._active_connection_test_ids.pop(endpoint_id, None)
        index = self.endpoint_selector.findText(endpoint_id)
        if index >= 0:
            self.endpoint_selector.removeItem(index)
        for combo in self._role_endpoint_combos.values():
            index = combo.findText(endpoint_id)
            if index >= 0:
                combo.removeItem(index)
        self.endpoint_selector.setCurrentText("DEFAULT")

    def _test_current_endpoint(self) -> None:
        endpoint_id = self._current_endpoint_id()
        endpoint = self.state.model_endpoints.get(endpoint_id)
        if endpoint is not None:
            self._start_connection_test(
                endpoint_id, endpoint, self.test_model.text().strip())

    def _start_connection_test(
        self, endpoint_id: str, endpoint: ModelEndpointState, model: str
    ) -> str:
        request_id = uuid.uuid4().hex
        self._active_connection_test_ids[endpoint_id] = request_id
        button = self._connection_test_buttons.get(endpoint_id)
        if button is not None:
            button.setEnabled(False)
        self.connection_tester.test(request_id, endpoint, model)
        return request_id

    def _on_connection_test_finished(
        self,
        request_id: str,
        success: bool,
        category: str,
        message: str,
    ) -> None:
        for endpoint_id, active_id in self._active_connection_test_ids.items():
            if request_id != active_id:
                continue
            self._active_connection_test_ids[endpoint_id] = None
            button = self._connection_test_buttons.get(endpoint_id)
            current_id = (self._current_endpoint_id()
                          if hasattr(self, "endpoint_selector") else endpoint_id)
            visible_button = getattr(self, "test_connection_button", None)
            if button is not None and (
                    button is not visible_button or endpoint_id == current_id):
                button.setEnabled(True)
            self._show_connection_test_result(
                endpoint_id, success, category, message)
            return

    def _show_connection_test_result(
        self,
        endpoint_id: str,
        success: bool,
        category: str,
        message: str,
    ) -> None:
        if success:
            InfoBar.success(
                f"{endpoint_id} 连接成功", message, parent=self,
                position=InfoBarPosition.TOP, duration=3000)
            return
        InfoBar.error(
            f"{endpoint_id} 连接失败", message, parent=self,
            position=InfoBarPosition.TOP, duration=5000)
