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
from typing import Any, Dict

from pet_registry import _app_data_dir  # 复用同一 AppData 根以放 launch_config.json

TEMPLATE_PATH = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "config",
    "default_common_config.example.json",
)


def load_template() -> Dict[str, Any]:
    """加载默认配置模板。文件不存在则抛错（仓库应自带 .example）。"""
    with open(TEMPLATE_PATH, "r", encoding="utf-8") as f:
        return json.load(f)


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
    if "provider" in settings:
        profile["provider"] = settings["provider"]
    if "baseUrl" in settings:
        profile["baseUrl"] = settings["baseUrl"]
    if "apiKey" in settings:
        profile["apiKey"] = settings["apiKey"]
    if "model" in settings:
        profile["model"] = settings["model"]
    if "visualModel" in settings:
        profile["visual_model"] = settings["visualModel"]  # 真实字段名下划线
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
    os.makedirs(_app_data_dir(), exist_ok=True)
    out_path = os.path.join(_app_data_dir(), "launch_config.json")
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(cfg, f, indent=2, ensure_ascii=False)
    return out_path


def _clamp_int(value: Any, lo: int, hi: int, default: int) -> int:
    try:
        v = int(value)
    except (TypeError, ValueError):
        return default
    return max(lo, min(hi, v))
