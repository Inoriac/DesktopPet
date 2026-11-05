"""启动器运行期配置状态。

各页面控件读写 AppState 单例的字段；启动时由 main.py 收集后交给
config_loader.apply_settings 导出。字段名与 config_loader.apply_settings 的分支对应。
"""

from __future__ import annotations

from dataclasses import dataclass, field, asdict
from typing import Any, Dict


MODEL_ROLES = (
    "dialogue", "vision", "fastExtract", "consolidation", "diary", "daydream",
    "agencyDecision", "taskExecution", "personalityEvolution")
DEFAULT_ANTHROPIC_VERSION = "2023-06-01"


@dataclass
class ModelEndpointState:
    provider: str = "openai-compatible"
    base_url: str = ""
    api_key: str = ""
    anthropic_version: str = DEFAULT_ANTHROPIC_VERSION
    extra_headers: Dict[str, str] = field(default_factory=dict)


@dataclass
class ModelRoleState:
    endpoint_ref: str = "DEFAULT"
    model: str = ""


def _default_model_endpoints() -> Dict[str, ModelEndpointState]:
    return {"DEFAULT": ModelEndpointState()}


def _default_model_roles() -> Dict[str, ModelRoleState]:
    return {role: ModelRoleState() for role in MODEL_ROLES}


@dataclass
class AppState:
    # 宠物设置
    pet_name: str = "Milltina"
    pet_profile_id: str = ""
    scale_percent: int = 100
    always_on_top: bool = True
    click_through: bool = False

    # AI 设置
    ai_enabled: bool = False
    autonomy_enabled: bool = False
    emotion_enabled: bool = True
    auto_screen_chat: bool = False
    chat_interval_min_ms: int = 480000
    chat_interval_max_ms: int = 720000
    active_endpoint: str = "DEFAULT"
    model_endpoints: Dict[str, ModelEndpointState] = field(
        default_factory=_default_model_endpoints)
    model_roles: Dict[str, ModelRoleState] = field(
        default_factory=_default_model_roles)
    endpoint_models: Dict[str, str] = field(default_factory=dict)
    endpoint_role_presets: Dict[str, Dict[str, ModelRoleState]] = field(
        default_factory=dict)

    # 语音设置（音效开关暂不实现，见计划 §十一）
    voice_enabled: bool = False
    speaker: str = "feibi"
    volume_percent: int = 75

    # 气泡设置
    bubble_opacity: int = 80
    bubble_font_size: int = 14
    bubble_offset_x: int = 0
    bubble_offset_y: int = -20

    # 高级-渲染
    antialiasing: bool = True
    shadow_quality: str = "medium"
    texture_quality: str = "high"
    # 高级-交互
    drag_threshold: int = 5
    click_timeout: int = 200
    # 高级-窗口吸附
    snap_enabled: bool = True
    snap_threshold: int = 30
    snap_vertical_offset: int = 0

    # 主题（light/dark，经 QSettings 与 C++ 共享）
    theme: str = "dark"

    def __post_init__(self) -> None:
        self.sync_endpoint_presets()

    def sync_endpoint_presets(self) -> None:
        """Keep active model_roles and per-endpoint role presets consistent."""
        self.model_endpoints = dict(self.model_endpoints)
        self.model_endpoints.setdefault("DEFAULT", ModelEndpointState())
        if (not isinstance(self.active_endpoint, str)
                or self.active_endpoint not in self.model_endpoints):
            self.active_endpoint = "DEFAULT"

        self.model_roles = dict(self.model_roles)
        for role in MODEL_ROLES:
            role_state = self.model_roles.get(role)
            if isinstance(role_state, ModelRoleState):
                self.model_roles[role] = ModelRoleState(
                    role_state.endpoint_ref, role_state.model)
            elif isinstance(role_state, dict):
                ref = role_state.get(
                    "endpoint_ref",
                    role_state.get("endpointRef", self.active_endpoint),
                )
                mdl = role_state.get("model", "")
                self.model_roles[role] = ModelRoleState(
                    ref if isinstance(ref, str) else self.active_endpoint,
                    mdl if isinstance(mdl, str) else "",
                )
            else:
                self.model_roles[role] = ModelRoleState(
                    endpoint_ref=self.active_endpoint)

        self.endpoint_models = {
            ep_id: model
            for ep_id, model in dict(self.endpoint_models).items()
            if ep_id in self.model_endpoints and isinstance(model, str)
        }

        cleaned_presets: Dict[str, Dict[str, ModelRoleState]] = {}
        for ep_id, preset in dict(self.endpoint_role_presets).items():
            if ep_id not in self.model_endpoints or not isinstance(preset, dict):
                continue
            ep_default = self.endpoint_models.get(ep_id, "")
            normalized: Dict[str, ModelRoleState] = {}
            for role in MODEL_ROLES:
                item = preset.get(role)
                if isinstance(item, ModelRoleState):
                    ref = (item.endpoint_ref
                           if item.endpoint_ref in self.model_endpoints
                           else ep_id)
                    normalized[role] = ModelRoleState(ref, item.model)
                elif isinstance(item, dict):
                    raw_ref = item.get(
                        "endpoint_ref", item.get("endpointRef", ep_id))
                    ref = (raw_ref
                           if isinstance(raw_ref, str)
                           and raw_ref in self.model_endpoints
                           else ep_id)
                    raw_model = item.get(
                        "model", ep_default if ref == ep_id else "")
                    normalized[role] = ModelRoleState(
                        ref, raw_model if isinstance(raw_model, str) else "")
                else:
                    normalized[role] = ModelRoleState(ep_id, ep_default)
            cleaned_presets[ep_id] = normalized

        cleaned_presets[self.active_endpoint] = {
            role: ModelRoleState(
                self.model_roles[role].endpoint_ref,
                self.model_roles[role].model,
            )
            for role in MODEL_ROLES
        }

        for ep_id in self.model_endpoints:
            if not self.endpoint_models.get(ep_id):
                source_roles = cleaned_presets.get(ep_id, {})
                inferred = next(
                    (r.model for r in source_roles.values()
                     if r.endpoint_ref == ep_id and r.model),
                    next(
                        (r.model for r in self.model_roles.values()
                         if r.endpoint_ref == ep_id and r.model),
                        "",
                    ),
                )
                self.endpoint_models[ep_id] = inferred
            if ep_id not in cleaned_presets:
                default_model = self.endpoint_models.get(ep_id, "")
                cleaned_presets[ep_id] = {
                    role: ModelRoleState(ep_id, default_model)
                    for role in MODEL_ROLES
                }

        self.endpoint_role_presets = cleaned_presets

    @classmethod
    def from_config(cls, config: Dict[str, Any] | None) -> "AppState":
        """Restore launcher-managed fields from a previously exported config."""
        state = cls()
        if not isinstance(config, dict):
            return state

        def object_at(parent: Any, key: str) -> Dict[str, Any]:
            value = parent.get(key) if isinstance(parent, dict) else None
            return value if isinstance(value, dict) else {}

        def set_bool(name: str, parent: Dict[str, Any], key: str) -> None:
            value = parent.get(key)
            if isinstance(value, bool):
                setattr(state, name, value)

        def set_int(name: str, parent: Dict[str, Any], key: str) -> None:
            value = parent.get(key)
            if isinstance(value, (int, float)) and not isinstance(value, bool):
                setattr(state, name, int(value))

        def set_text(name: str, parent: Dict[str, Any], key: str) -> None:
            value = parent.get(key)
            if isinstance(value, str):
                setattr(state, name, value)

        def role_routes(profile: Dict[str, Any], role: str) -> list[Dict[str, Any]]:
            model_roles = object_at(profile, "modelRoles")
            role_config = object_at(model_roles, role)
            routes = role_config.get("routes")
            if not isinstance(routes, list):
                return []
            return [route for route in routes if isinstance(route, dict)]

        def endpoint_from_route(
            route: Dict[str, Any], defaults: ModelEndpointState | None = None
        ) -> ModelEndpointState:
            endpoint = ModelEndpointState() if defaults is None else ModelEndpointState(
                provider=defaults.provider,
                base_url=defaults.base_url,
                api_key=defaults.api_key,
                anthropic_version=defaults.anthropic_version,
                extra_headers=dict(defaults.extra_headers),
            )
            values = {
                "provider": "provider",
                "base_url": "baseUrl",
                "api_key": "apiKey",
                "anthropic_version": "anthropicVersion",
            }
            for field_name, route_name in values.items():
                value = route.get(route_name)
                if isinstance(value, str):
                    if (field_name == "anthropic_version" and
                            not value.strip()):
                        continue
                    setattr(endpoint, field_name, value)
            if "extraHeaders" in route:
                headers = route.get("extraHeaders")
                endpoint.extra_headers = {}
            else:
                headers = None
            if isinstance(headers, dict):
                endpoint.extra_headers = {
                    str(key): value for key, value in headers.items()
                    if isinstance(key, str) and isinstance(value, str)
                }
            return endpoint

        def endpoint_from_legacy(profile: Dict[str, Any]) -> ModelEndpointState:
            return endpoint_from_route(profile)

        def fallback_model(profile: Dict[str, Any], role: str) -> str:
            if role == "vision":
                value = profile.get("visual_model", "")
            elif role == "daydream":
                value = object_at(profile, "daydream").get(
                    "model", profile.get("model", ""))
            else:
                value = profile.get("model", "")
            return value if isinstance(value, str) else ""

        pet = object_at(config, "petSettings")
        set_int("scale_percent", pet, "scalePercent")
        set_bool("always_on_top", pet, "alwaysOnTop")
        set_bool("click_through", pet, "clickThrough")

        ai = object_at(config, "aiSettings")
        profiles = object_at(ai, "profiles")
        active_profile = ai.get("activeProfile", "default")
        profile = object_at(
            profiles, active_profile if isinstance(active_profile, str) else "default")
        if not profile and not profiles:
            profile = ai
        set_bool("ai_enabled", profile, "enabled")
        autonomy = object_at(object_at(config, "aiSettings"), "autonomy")
        autonomy_enabled = autonomy.get("enabled")
        autonomy_paused = autonomy.get("paused")
        if isinstance(autonomy_enabled, bool):
            state.autonomy_enabled = autonomy_enabled and not (
                autonomy_paused if isinstance(autonomy_paused, bool) else False)
        registry = profile.get("modelEndpoints")
        state.model_endpoints = {}
        state.model_roles = {}
        state.endpoint_models = {}
        state.endpoint_role_presets = {}
        if isinstance(registry, dict) and registry:
            for endpoint_id, raw_endpoint in registry.items():
                if isinstance(endpoint_id, str) and isinstance(raw_endpoint, dict):
                    state.model_endpoints[endpoint_id] = endpoint_from_route(
                        raw_endpoint)
            state.model_endpoints.setdefault(
                "DEFAULT", endpoint_from_legacy(profile))
            for role in MODEL_ROLES:
                routes = role_routes(profile, role)
                first = routes[0] if routes else {}
                endpoint_ref = first.get("endpointRef", "DEFAULT")
                model = first.get("model", fallback_model(profile, role))
                state.model_roles[role] = ModelRoleState(
                    endpoint_ref=(endpoint_ref if isinstance(endpoint_ref, str)
                                  else "DEFAULT"),
                    model=model if isinstance(model, str) else "",
                )
            raw_active = profile.get("activeEndpoint")
            if isinstance(raw_active, str) and raw_active in state.model_endpoints:
                state.active_endpoint = raw_active
            else:
                dlg_ref = state.model_roles["dialogue"].endpoint_ref
                state.active_endpoint = (
                    dlg_ref if dlg_ref in state.model_endpoints else "DEFAULT")
            for endpoint_id, raw_endpoint in registry.items():
                if not isinstance(endpoint_id, str) or not isinstance(raw_endpoint, dict):
                    continue
                raw_default_model = raw_endpoint.get("defaultModel")
                if isinstance(raw_default_model, str):
                    state.endpoint_models[endpoint_id] = raw_default_model
                raw_preset = raw_endpoint.get("modelRoles")
                if (endpoint_id != state.active_endpoint
                        and isinstance(raw_preset, dict) and raw_preset):
                    fallback_default = state.endpoint_models.get(endpoint_id, "")
                    preset_roles: Dict[str, ModelRoleState] = {}
                    for role in MODEL_ROLES:
                        item = raw_preset.get(role)
                        if isinstance(item, dict):
                            ref = item.get("endpointRef", endpoint_id)
                            if (not isinstance(ref, str)
                                    or ref not in state.model_endpoints):
                                ref = endpoint_id
                            mdl = item.get(
                                "model",
                                fallback_default if ref == endpoint_id else "",
                            )
                            preset_roles[role] = ModelRoleState(
                                endpoint_ref=ref,
                                model=mdl if isinstance(mdl, str) else "",
                            )
                        else:
                            preset_roles[role] = ModelRoleState(
                                endpoint_ref=endpoint_id,
                                model=fallback_default,
                            )
                    state.endpoint_role_presets[endpoint_id] = preset_roles
        else:
            dialogue_routes = role_routes(profile, "dialogue")
            profile_endpoint = endpoint_from_legacy(profile)
            state.model_endpoints["DEFAULT"] = endpoint_from_route(
                dialogue_routes[0], profile_endpoint
            ) if dialogue_routes else profile_endpoint
            for role in MODEL_ROLES:
                routes = role_routes(profile, role)
                endpoint_ref = "DEFAULT"
                for index, route in enumerate(routes):
                    effective_endpoint = endpoint_from_route(
                        route, profile_endpoint)
                    if role == "dialogue" and index == 0:
                        migrated_id = "DEFAULT"
                    elif effective_endpoint == state.model_endpoints["DEFAULT"]:
                        migrated_id = "DEFAULT"
                    else:
                        migrated_id = f"MIGRATED_{role.upper()}_{index}"
                        state.model_endpoints[migrated_id] = effective_endpoint
                    if index == 0:
                        endpoint_ref = migrated_id
                first = routes[0] if routes else {}
                model = first.get("model", fallback_model(profile, role))
                state.model_roles[role] = ModelRoleState(
                    endpoint_ref=endpoint_ref,
                    model=model if isinstance(model, str) else "",
                )
            state.active_endpoint = "DEFAULT"
        state.sync_endpoint_presets()

        emotion = object_at(profile, "emotion")
        set_bool("emotion_enabled", emotion, "enabled")
        screen_chat = object_at(profile, "screenChat")
        set_bool("auto_screen_chat", screen_chat, "enabled")
        set_int("chat_interval_min_ms", screen_chat, "minIntervalMs")
        set_int("chat_interval_max_ms", screen_chat, "maxIntervalMs")
        set_int("bubble_opacity", screen_chat, "bubbleOpacityPercent")
        set_int("bubble_font_size", screen_chat, "bubbleFontSize")
        set_int("bubble_offset_x", screen_chat, "bubbleOffsetX")
        set_int("bubble_offset_y", screen_chat, "bubbleOffsetY")

        voice = object_at(profile, "voice")
        set_bool("voice_enabled", voice, "enabled")
        set_text("speaker", voice, "selectedSpeaker")
        volume = object_at(config, "appSettings").get("volume")
        if isinstance(volume, (int, float)) and not isinstance(volume, bool):
            state.volume_percent = round(float(volume) * 100)

        render = object_at(config, "renderSettings")
        set_bool("antialiasing", render, "antialiasing")
        set_text("shadow_quality", render, "shadowQuality")
        set_text("texture_quality", render, "textureQuality")
        interaction = object_at(config, "interactionSettings")
        set_int("drag_threshold", interaction, "dragThreshold")
        set_int("click_timeout", interaction, "clickTimeout")
        snapping = object_at(interaction, "windowSnapping")
        set_bool("snap_enabled", snapping, "enabled")
        set_int("snap_threshold", snapping, "snapThreshold")
        set_int("snap_vertical_offset", snapping, "verticalOffset")
        return state

    def to_settings_dict(self) -> Dict[str, Any]:
        """映射到 config_loader.apply_settings 期望的扁平 settings 键名。"""
        self.sync_endpoint_presets()
        d = asdict(self)
        # 角色身份走启动参数，不进模型配置。
        d.pop("pet_name", None)
        d.pop("pet_profile_id", None)
        d.pop("theme", None)
        # 键名转换：snake_case → apply_settings 分支键名
        return {
            "scalePercent": d["scale_percent"],
            "alwaysOnTop": d["always_on_top"],
            "clickThrough": d["click_through"],
            "aiEnabled": d["ai_enabled"],
            "autonomyEnabled": d["autonomy_enabled"],
            "emotionEnabled": d["emotion_enabled"],
            "autoScreenChat": d["auto_screen_chat"],
            "chatIntervalMinMs": d["chat_interval_min_ms"],
            "chatIntervalMaxMs": d["chat_interval_max_ms"],
            "activeEndpoint": d["active_endpoint"],
            "modelEndpoints": d["model_endpoints"],
            "modelRoles": d["model_roles"],
            "endpointModels": d["endpoint_models"],
            "endpointRolePresets": d["endpoint_role_presets"],
            "voiceEnabled": d["voice_enabled"],
            "speaker": d["speaker"],
            "volumePercent": d["volume_percent"],
            "bubbleOpacity": d["bubble_opacity"],
            "bubbleFontSize": d["bubble_font_size"],
            "bubbleOffsetX": d["bubble_offset_x"],
            "bubbleOffsetY": d["bubble_offset_y"],
        }

    def to_advanced_overrides(self) -> Dict[str, Any]:
        """高级设置直接覆盖模板副本里的对应字段（不在 apply_settings 内）。"""
        return {
            "renderSettings": {
                "antialiasing": self.antialiasing,
                "shadowQuality": self.shadow_quality,
                "textureQuality": self.texture_quality,
            },
            "interactionSettings": {
                "dragThreshold": self.drag_threshold,
                "clickTimeout": self.click_timeout,
                "windowSnapping": {
                    "enabled": self.snap_enabled,
                    "snapThreshold": self.snap_threshold,
                    "verticalOffset": self.snap_vertical_offset,
                },
            },
        }
