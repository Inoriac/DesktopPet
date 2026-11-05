"""配置加载与导出。

导出的 launch_config.json 以 config/default_common_config.example.json 为结构基线。
策略：先完整加载模板，仅覆盖用户向字段，再整体导出，避免 colliders/相机/behaviorPolicy
等内部字段丢失（loadConfig 为整体替换语义）。
"""

from __future__ import annotations

import copy
import json
import os
import re
from typing import Any, Dict

from app_state import DEFAULT_ANTHROPIC_VERSION, MODEL_ROLES
from atomic_write import atomic_write_text

TEMPLATE_PATH = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "config",
    "default_common_config.example.json",
)
SAVED_CONFIG_NAME = "launch_config.json"
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
    candidates = [
        saved_config_path(),
        os.path.join(_fallback_config_dir(), SAVED_CONFIG_NAME),
    ]
    for path in candidates:
        try:
            with open(path, "r", encoding="utf-8") as handle:
                config = json.load(handle)
        except (FileNotFoundError, OSError, json.JSONDecodeError):
            continue
        if isinstance(config, dict):
            return config
    return None


def merge_with_template(
    template: Dict[str, Any], saved: Dict[str, Any] | None
) -> Dict[str, Any]:
    """Merge a possibly partial saved config over a complete schema template."""
    merged = copy.deepcopy(template)
    if not isinstance(saved, dict):
        return merged

    def merge_object(target: Dict[str, Any], source: Dict[str, Any]) -> None:
        for key, value in source.items():
            current = target.get(key)
            if isinstance(current, dict):
                if isinstance(value, dict):
                    merge_object(current, value)
                continue
            target[key] = copy.deepcopy(value)

    merge_object(merged, saved)
    return merged


def load_effective_config() -> Dict[str, Any]:
    """Load saved values while restoring any schema fields that are missing."""
    return merge_with_template(load_template(), load_saved_config())


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
        value = str(_state_value(endpoint, state_name, ""))
        if state_name == "anthropic_version":
            value = value.strip() or DEFAULT_ANTHROPIC_VERSION
        output[route_name] = value
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
    *,
    active_endpoint: str | None = None,
    endpoint_models: Dict[str, Any] | None = None,
    endpoint_role_presets: Dict[str, Any] | None = None,
) -> None:
    """Write endpoint registry and role references without duplicating secrets."""
    if not isinstance(endpoints, dict) or "DEFAULT" not in endpoints:
        raise ValueError("modelEndpoints must contain DEFAULT")
    previous_endpoints = (
        profile.get("modelEndpoints")
        if isinstance(profile.get("modelEndpoints"), dict)
        else {}
    )
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
        # Launcher exposes no per-route enable toggle. A role managed here must
        # therefore be usable even when an older saved route was disabled.
        first["enabled"] = True
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

    if isinstance(active_endpoint, str) and active_endpoint in serialized_endpoints:
        resolved_active = active_endpoint
    elif (isinstance(profile.get("activeEndpoint"), str)
            and profile["activeEndpoint"] in serialized_endpoints):
        resolved_active = profile["activeEndpoint"]
    else:
        dlg_routes = (
            profile.get("modelRoles", {})
            .get("dialogue", {})
            .get("routes", [])
        )
        dlg_ref = (
            dlg_routes[0].get("endpointRef")
            if isinstance(dlg_routes, list)
            and dlg_routes
            and isinstance(dlg_routes[0], dict)
            else "DEFAULT"
        )
        resolved_active = (
            dlg_ref
            if isinstance(dlg_ref, str) and dlg_ref in serialized_endpoints
            else "DEFAULT"
        )

    active_role_map: Dict[str, tuple[str, str]] = {}
    for role in MODEL_ROLES:
        role_cfg = profile.get("modelRoles", {}).get(role)
        routes = role_cfg.get("routes") if isinstance(role_cfg, dict) else None
        if isinstance(routes, list) and routes and isinstance(routes[0], dict):
            ep_ref = routes[0].get("endpointRef", resolved_active)
            mdl = routes[0].get("model", "")
            active_role_map[role] = (
                ep_ref if isinstance(ep_ref, str) and ep_ref in serialized_endpoints else resolved_active,
                mdl if isinstance(mdl, str) else "",
            )

    for endpoint_id, endpoint_obj in serialized_endpoints.items():
        prev_ep = (
            previous_endpoints.get(endpoint_id)
            if isinstance(previous_endpoints.get(endpoint_id), dict)
            else {}
        )
        explicit_default = (
            endpoint_models.get(endpoint_id)
            if isinstance(endpoint_models, dict)
            else None
        )
        if isinstance(explicit_default, str):
            default_model = explicit_default
        elif isinstance(prev_ep.get("defaultModel"), str):
            default_model = prev_ep["defaultModel"]
        else:
            default_model = ""

        raw_preset = None
        if (endpoint_id != resolved_active
                and isinstance(endpoint_role_presets, dict)
                and isinstance(endpoint_role_presets.get(endpoint_id), dict)):
            raw_preset = endpoint_role_presets[endpoint_id]
        elif (endpoint_id != resolved_active
                and isinstance(prev_ep.get("modelRoles"), dict)):
            raw_preset = prev_ep["modelRoles"]

        serialized_preset: Dict[str, Dict[str, str]] = {}
        for role in MODEL_ROLES:
            if endpoint_id == resolved_active:
                if role in active_role_map:
                    ep_ref, mdl = active_role_map[role]
                else:
                    ep_ref, mdl = endpoint_id, default_model
            elif isinstance(raw_preset, dict) and role in raw_preset:
                item = raw_preset[role]
                cand_ref = str(_state_value(
                    item, "endpoint_ref",
                    _state_value(item, "endpointRef", endpoint_id)))
                ep_ref = cand_ref if cand_ref in serialized_endpoints else endpoint_id
                mdl = str(_state_value(
                    item, "model", default_model if ep_ref == endpoint_id else ""))
            else:
                ep_ref, mdl = endpoint_id, default_model
            serialized_preset[role] = {"endpointRef": ep_ref, "model": mdl}

        if not default_model:
            default_model = next(
                (entry["model"] for entry in serialized_preset.values()
                 if entry["endpointRef"] == endpoint_id and entry["model"]),
                next(
                    (mdl for ref, mdl in active_role_map.values()
                     if ref == endpoint_id and mdl),
                    "",
                ),
            )
            if endpoint_id != resolved_active and raw_preset is None:
                for role in MODEL_ROLES:
                    serialized_preset[role]["model"] = default_model

        endpoint_obj["defaultModel"] = default_model
        endpoint_obj["modelRoles"] = serialized_preset

    profile["activeEndpoint"] = resolved_active
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
    autonomy = cfg.setdefault("aiSettings", {}).setdefault("autonomy", {})
    if "autonomyEnabled" in settings:
        autonomy["enabled"] = bool(settings["autonomyEnabled"])
        autonomy["paused"] = False
    profile = _profile(cfg)
    if "aiEnabled" in settings:
        profile["enabled"] = bool(settings["aiEnabled"])
    if "emotionEnabled" in settings:
        profile.setdefault("emotion", {})["enabled"] = bool(settings["emotionEnabled"])
    if ("modelEndpoints" in settings or "modelRoles" in settings
            or "activeEndpoint" in settings
            or "endpointRolePresets" in settings):
        apply_model_endpoint_settings(
            profile,
            settings.get("modelEndpoints", {}),
            settings.get("modelRoles", {}),
            active_endpoint=settings.get("activeEndpoint"),
            endpoint_models=settings.get("endpointModels"),
            endpoint_role_presets=settings.get("endpointRolePresets"),
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


def _fallback_config_dir() -> str:
    return os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "runtime",
        "launcher",
    )


def export_config(cfg: Dict[str, Any]) -> str:
    """导出到 AppData/launch_config.json，返回绝对路径（供 --config 传给 C++）。"""
    payload = json.dumps(cfg, indent=2, ensure_ascii=False)
    directory = _app_data_dir()
    out_path = saved_config_path()
    try:
        os.makedirs(directory, exist_ok=True)
        atomic_write_text(
            out_path,
            payload,
            prefix=".launch_config-",
        )
        return out_path
    except PermissionError:
        fallback_dir = _fallback_config_dir()
        os.makedirs(fallback_dir, exist_ok=True)
        fallback_path = os.path.join(fallback_dir, SAVED_CONFIG_NAME)
        atomic_write_text(
            fallback_path,
            payload,
            prefix=".launch_config-",
        )
        return fallback_path


def _clamp_int(value: Any, lo: int, hi: int, default: int) -> int:
    try:
        v = int(value)
    except (TypeError, ValueError):
        return default
    return max(lo, min(hi, v))
