"""配置加载与导出。

遵循前端修改计划 §三：导出的 launch_config.json 必须与
config/default_common_config.example.json 同构（四大块 + petSettings 新增块）。
策略：先完整加载模板，仅覆盖用户向字段，再整体导出，避免 colliders/相机/behaviorPolicy
等内部字段丢失（loadConfig 为整体替换语义）。
"""

from __future__ import annotations

import copy
import json
import os
import re
import tempfile
from typing import Any, Dict

TEMPLATE_PATH = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "config",
    "default_common_config.example.json",
)
SAVED_CONFIG_NAME = "launch_config.json"
MODEL_ROLES = (
    "dialogue", "vision", "fastExtract", "consolidation", "diary", "daydream")
ENDPOINT_ID_PATTERN = re.compile(r"^[A-Za-z][A-Za-z0-9_-]{0,63}$")
INLINE_CONNECTION_FIELDS = (
    "provider", "baseUrl", "apiKey", "anthropicVersion", "extraHeaders")


def _app_data_dir() -> str:
    from pet_registry import _app_data_dir as app_data_dir
    return app_data_dir()


def load_template() -> Dict[str, Any]:
    """加载默认配置模板。文件不存在则抛错（仓库应自带 .example）。"""
    with open(TEMPLATE_PATH, "r", encoding="utf-8") as f:
        return json.load(f)


def saved_config_path() -> str:
    return os.path.join(_app_data_dir(), SAVED_CONFIG_NAME)


def load_saved_config() -> Dict[str, Any] | None:
    """Load the last explicitly saved/exported launcher configuration."""
    try:
        with open(saved_config_path(), "r", encoding="utf-8") as handle:
            config = json.load(handle)
    except (FileNotFoundError, OSError, json.JSONDecodeError):
        return None
    return config if isinstance(config, dict) else None


def _profile(cfg: Dict[str, Any]) -> Dict[str, Any]:
    """取 activeProfile 对应的 profile 对象（默认 'default'），供用户向字段写入。"""
    ai = cfg.setdefault("aiSettings", {})
    active_profile = ai.setdefault("activeProfile", "default")
    profiles = ai.setdefault("profiles", {})
    return profiles.setdefault(active_profile, {})


def _screen_chat(profile: Dict[str, Any]) -> Dict[str, Any]:
    return profile.setdefault("screenChat", {})


def _voice(profile: Dict[str, Any]) -> Dict[str, Any]:
    return profile.setdefault("voice", {})


def _first_model_role_route(
    profile: Dict[str, Any], role: str, route_id: str, supports_vision: bool
) -> Dict[str, Any]:
    """Return the UI-managed first route without replacing existing fallbacks."""
    roles = profile.setdefault("modelRoles", {})
    role_config = roles.setdefault(role, {})
    routes = role_config.setdefault("routes", [])
    if not routes:
        routes.append({
            "routeId": route_id,
            "enabled": True,
            "supportsVision": supports_vision,
        })
    first = routes[0]
    first.setdefault("routeId", route_id)
    first.setdefault("enabled", True)
    first.setdefault("supportsVision", supports_vision)
    return first


def _state_value(state: Any, name: str, default: Any = "") -> Any:
    if isinstance(state, dict):
        return state.get(name, default)
    return getattr(state, name, default)


def _endpoint_json(endpoint: Any) -> Dict[str, Any]:
    fields = {
        "provider": "provider",
        "base_url": "baseUrl",
        "api_key": "apiKey",
        "anthropic_version": "anthropicVersion",
    }
    output = {}
    for state_name, route_name in fields.items():
        output[route_name] = str(_state_value(endpoint, state_name, ""))
    headers = _state_value(endpoint, "extra_headers", {})
    if isinstance(headers, dict):
        output["extraHeaders"] = {
            str(key): value for key, value in headers.items()
            if isinstance(key, str) and isinstance(value, str)
        }
    return output


def _migrated_endpoint_id(role: str, index: int) -> str:
    if role == "dialogue" and index == 0:
        return "DEFAULT"
    return f"MIGRATED_{role.upper()}_{index}"


def _inline_connection_matches_default(
    route: Dict[str, Any], default_endpoint: Dict[str, Any]
) -> bool:
    effective = copy.deepcopy(default_endpoint)
    for field in INLINE_CONNECTION_FIELDS:
        if field not in route:
            continue
        value = route[field]
        if field == "extraHeaders":
            effective[field] = {
                str(key): item for key, item in value.items()
                if isinstance(key, str) and isinstance(item, str)
            } if isinstance(value, dict) else {}
        elif isinstance(value, str):
            effective[field] = value
    return effective == default_endpoint


def apply_model_endpoint_settings(
    profile: Dict[str, Any],
    endpoints: Dict[str, Any],
    roles: Dict[str, Any],
) -> None:
    """Write endpoint registry and role references without duplicating secrets."""
    if not isinstance(endpoints, dict) or "DEFAULT" not in endpoints:
        raise ValueError("modelEndpoints must contain DEFAULT")
    serialized_endpoints = {}
    for endpoint_id, endpoint in endpoints.items():
        if not isinstance(endpoint_id, str) or not ENDPOINT_ID_PATTERN.fullmatch(
                endpoint_id):
            raise ValueError(f"invalid model endpoint ID: {endpoint_id!r}")
        serialized_endpoints[endpoint_id] = _endpoint_json(endpoint)

    model_roles = profile.setdefault("modelRoles", {})
    for role, role_config in list(model_roles.items()):
        if not isinstance(role_config, dict):
            continue
        routes = role_config.get("routes")
        if not isinstance(routes, list):
            continue
        for index, route in enumerate(routes):
            if not isinstance(route, dict):
                continue
            if index == 0 and role in roles:
                for field in INLINE_CONNECTION_FIELDS:
                    route.pop(field, None)
                continue
            endpoint_ref = route.get("endpointRef")
            if isinstance(endpoint_ref, str) and endpoint_ref:
                if endpoint_ref not in serialized_endpoints:
                    raise ValueError(
                        f"model route {role}[{index}] references unknown "
                        f"endpoint {endpoint_ref}")
            else:
                migrated_id = _migrated_endpoint_id(role, index)
                if migrated_id in serialized_endpoints:
                    route["endpointRef"] = migrated_id
                elif _inline_connection_matches_default(
                        route, serialized_endpoints["DEFAULT"]):
                    route["endpointRef"] = "DEFAULT"
                else:
                    raise ValueError(
                        f"missing migrated endpoint {migrated_id} for {role}[{index}]")
            for field in INLINE_CONNECTION_FIELDS:
                route.pop(field, None)

    daydream = profile.get("daydream")
    legacy_daydream = daydream if isinstance(daydream, dict) else {}
    for role, state in roles.items():
        if role not in MODEL_ROLES:
            continue
        endpoint_ref = str(_state_value(state, "endpoint_ref", "DEFAULT"))
        if endpoint_ref not in serialized_endpoints:
            raise ValueError(
                f"model role {role} references unknown endpoint {endpoint_ref}")
        model = str(_state_value(state, "model", ""))
        first = _first_model_role_route(
            profile, role, f"{role}-primary", role == "vision")
        first["endpointRef"] = endpoint_ref
        first["model"] = model
        first["supportsVision"] = role == "vision"
        for field in INLINE_CONNECTION_FIELDS:
            first.pop(field, None)
        if role == "daydream":
            if "maxTokens" in legacy_daydream:
                first.setdefault("maxTokens", legacy_daydream["maxTokens"])
            if "temperature" in legacy_daydream:
                first.setdefault("temperature", legacy_daydream["temperature"])

    if isinstance(daydream, dict):
        for legacy_field in ("model", "maxTokens", "temperature"):
            daydream.pop(legacy_field, None)

    profile["modelEndpoints"] = serialized_endpoints
    for legacy_field in (
            "provider", "baseUrl", "apiKey", "model", "visual_model",
            "anthropicVersion", "extraHeaders"):
        profile.pop(legacy_field, None)


def apply_settings(template: Dict[str, Any], settings: Dict[str, Any]) -> Dict[str, Any]:
    """将用户向 settings 覆盖到模板副本上，返回新对象（不修改入参）。

    settings 键命名见 apply_user_value 的各分支，覆盖 UI 控件到真实 schema 字段。
    缺失键不覆盖（沿用模板/默认）。
    """
    cfg = copy.deepcopy(template)

    # —— 宠物设置（petSettings：launcher 新增块）——
    pet = cfg.setdefault("petSettings", {})
    if "scalePercent" in settings:
        pet["scalePercent"] = _clamp_int(settings["scalePercent"], 50, 200, 100)
    if "alwaysOnTop" in settings:
        pet["alwaysOnTop"] = bool(settings["alwaysOnTop"])
    if "clickThrough" in settings:
        pet["clickThrough"] = bool(settings["clickThrough"])

    # —— AI 设置 ——
    profile = _profile(cfg)
    if "aiEnabled" in settings:
        profile["enabled"] = bool(settings["aiEnabled"])
    if "emotionEnabled" in settings:
        profile.setdefault("emotion", {})["enabled"] = bool(settings["emotionEnabled"])
    if "modelEndpoints" in settings or "modelRoles" in settings:
        apply_model_endpoint_settings(
            profile,
            settings.get("modelEndpoints", {}),
            settings.get("modelRoles", {}),
        )
    sc = _screen_chat(profile)
    if "autoScreenChat" in settings:
        sc["enabled"] = bool(settings["autoScreenChat"])
    if "chatIntervalMinMs" in settings:
        sc["minIntervalMs"] = _clamp_int(settings["chatIntervalMinMs"], 1000, 86400000, 480000)
    if "chatIntervalMaxMs" in settings:
        sc["maxIntervalMs"] = _clamp_int(settings["chatIntervalMaxMs"], 1000, 86400000, 720000)
    # 保证 min <= max
    if sc.get("minIntervalMs", 0) > sc.get("maxIntervalMs", 0):
        sc["minIntervalMs"], sc["maxIntervalMs"] = sc["maxIntervalMs"], sc["minIntervalMs"]
    # —— 气泡 ——（均挂 screenChat 下）
    if "bubbleOpacity" in settings:
        sc["bubbleOpacityPercent"] = _clamp_int(settings["bubbleOpacity"], 0, 100, 80)
    if "bubbleFontSize" in settings:
        sc["bubbleFontSize"] = _clamp_int(settings["bubbleFontSize"], 1, 200, 14)
    if "bubbleOffsetX" in settings:
        sc["bubbleOffsetX"] = int(settings["bubbleOffsetX"])
    if "bubbleOffsetY" in settings:
        sc["bubbleOffsetY"] = int(settings["bubbleOffsetY"])

    # —— 语音 ——
    vc = _voice(profile)
    if "voiceEnabled" in settings:
        vc["enabled"] = bool(settings["voiceEnabled"])
    if "speaker" in settings:
        # C++ 仅接受 feibi/mika/thirtyseven，否则回退 feibi
        vc["selectedSpeaker"] = settings["speaker"] if settings["speaker"] in (
            "feibi", "mika", "thirtyseven") else "feibi"

    # 音量走 appSettings.volume（0.0–1.0），UI 0–100% 互转
    if "volumePercent" in settings:
        cfg.setdefault("appSettings", {})["volume"] = round(
            _clamp_int(settings["volumePercent"], 0, 100, 75) / 100.0, 4)

    return cfg


def export_config(cfg: Dict[str, Any]) -> str:
    """导出到 AppData/launch_config.json，返回绝对路径（供 --config 传给 C++）。"""
    directory = _app_data_dir()
    os.makedirs(directory, exist_ok=True)
    out_path = saved_config_path()
    descriptor, temporary_path = tempfile.mkstemp(
        prefix=".launch_config-", suffix=".tmp", dir=directory)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
            json.dump(cfg, handle, indent=2, ensure_ascii=False)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary_path, out_path)
    except Exception:
        try:
            os.unlink(temporary_path)
        except OSError:
            pass
        raise
    return out_path


def _clamp_int(value: Any, lo: int, hi: int, default: int) -> int:
    try:
        v = int(value)
    except (TypeError, ValueError):
        return default
    return max(lo, min(hi, v))
