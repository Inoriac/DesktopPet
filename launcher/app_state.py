"""启动器运行期配置状态。

各页面控件读写 AppState 单例的字段；启动时由 main.py 收集后交给
config_loader.apply_settings 导出。字段名与 config_loader.apply_settings 的分支对应。
"""

from __future__ import annotations

from dataclasses import dataclass, field, asdict
from typing import Any, Dict


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
    emotion_enabled: bool = True
    auto_screen_chat: bool = False
    chat_interval_min_ms: int = 480000
    chat_interval_max_ms: int = 720000
    provider: str = "openai-compatible"
    base_url: str = "https://api.example.com/v1"
    api_key: str = ""
    model: str = ""
    visual_model: str = ""

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

        pet = object_at(config, "petSettings")
        set_int("scale_percent", pet, "scalePercent")
        set_bool("always_on_top", pet, "alwaysOnTop")
        set_bool("click_through", pet, "clickThrough")

        ai = object_at(config, "aiSettings")
        profiles = object_at(ai, "profiles")
        active_profile = ai.get("activeProfile", "default")
        profile = object_at(
            profiles, active_profile if isinstance(active_profile, str) else "default")
        set_bool("ai_enabled", profile, "enabled")
        set_text("provider", profile, "provider")
        set_text("base_url", profile, "baseUrl")
        set_text("api_key", profile, "apiKey")
        set_text("model", profile, "model")
        set_text("visual_model", profile, "visual_model")

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
            "emotionEnabled": d["emotion_enabled"],
            "autoScreenChat": d["auto_screen_chat"],
            "chatIntervalMinMs": d["chat_interval_min_ms"],
            "chatIntervalMaxMs": d["chat_interval_max_ms"],
            "provider": d["provider"],
            "baseUrl": d["base_url"],
            "apiKey": d["api_key"],
            "model": d["model"],
            "visualModel": d["visual_model"],
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
