"""Read-only profile statistics and presentation helpers for the launcher."""

from __future__ import annotations

from copy import deepcopy
from datetime import datetime, timedelta
import json
from pathlib import Path

from pet_registry import _app_data_dir, _is_canonical_uuid

COUNTERS = (
    "callCount", "successCount", "failureCount", "cancelledCount", "unknownUsageCount",
    "promptTokens", "completionTokens", "totalTokens", "reasoningTokens", "cachedTokens",
    "promptCacheHitTokens", "promptCacheMissTokens",
)
ROLE_LABELS = {
    "dialogue": "对话", "fastExtract": "快速提取", "consolidation": "记忆整理",
    "diary": "日记", "vision": "视觉", "daydream": "后台联想",
    "agencyDecision": "自主决策", "taskExecution": "任务执行",
    "personalityEvolution": "性格演化", "unclassified": "未分类",
}
RESULT_LABELS = {"success": "成功", "failure": "失败", "cancelled": "取消"}
EMOTION_LABELS = {
    "neutral": "平静", "joy": "愉快", "sadness": "低落", "anger": "生气",
    "fear": "害怕", "surprise": "惊讶", "disgust": "厌恶", "trust": "信任",
    "anticipation": "期待",
}


def profile_directory(profile_id: str) -> Path:
    if not _is_canonical_uuid(profile_id):
        raise ValueError("角色身份无效，无法读取统计")
    return Path(_app_data_dir()) / "profiles" / profile_id


def empty_counters() -> dict:
    return dict.fromkeys(COUNTERS, 0)


def _validate_statistics(statistics: dict) -> None:
    """Reject malformed persisted data instead of replacing the user's file."""
    if not isinstance(statistics, dict):
        raise ValueError("统计内容格式不正确")
    for key in ("allTime", "today", "session", "uptime"):
        if not isinstance(statistics.get(key, {}), dict):
            raise ValueError("统计计数格式不正确")
    rows = []
    for key in ("daily", "byModel", "recentCalls"):
        values = statistics.get(key, [])
        if not isinstance(values, list) or not all(isinstance(row, dict) for row in values):
            raise ValueError("统计明细格式不正确")
        rows.extend(values)
    rows.extend(statistics.get(key, {}) for key in ("allTime", "today", "session", "uptime"))
    for row in rows:
        for key in (*COUNTERS, "durationMs", "totalMs", "sessionMs", "sessionCount", "touchCount"):
            value = row.get(key, 0)
            if not isinstance(value, (int, float)) or isinstance(value, bool) or value < 0:
                raise ValueError("统计计数格式不正确")
        if "usage" in row:
            usage = row["usage"]
            if not isinstance(usage, dict) or any(
                not isinstance(usage.get(key, 0), (int, float)) or usage.get(key, 0) < 0
                for key in ("promptTokens", "completionTokens", "totalTokens")
            ):
                raise ValueError("调用用量格式不正确")


def offline_snapshot(snapshot: dict, now: datetime | None = None) -> dict:
    result = deepcopy(snapshot)
    statistics = result["statistics"]
    today = (now or datetime.now().astimezone()).astimezone().date().isoformat()
    statistics["today"] = next(
        ({**empty_counters(), **row} for row in statistics.get("daily", []) if row.get("date") == today),
        empty_counters(),
    )
    statistics["session"] = None
    statistics.setdefault("uptime", {})["sessionMs"] = None
    result["runtime"] = None
    result["emotion"] = {"available": False}
    return result


def load_saved_statistics(profile_id: str, now: datetime | None = None) -> tuple[dict | None, str]:
    try:
        path = profile_directory(profile_id) / "statistics.json"
        with path.open(encoding="utf-8") as source:
            saved = json.load(source)
        if not isinstance(saved, dict) or saved.get("version") != 2:
            raise ValueError("统计文件版本不受支持，请升级核心")
        if saved.get("profileId") != profile_id:
            raise ValueError("统计文件的角色身份不匹配")
        statistics = saved.get("statistics")
        _validate_statistics(statistics)
        return offline_snapshot({
            "schemaVersion": 1, "profileId": profile_id, "petName": saved.get("petName", ""),
            "capturedAt": saved.get("lastSaved", ""), "statistics": statistics,
        }, now), ""
    except FileNotFoundError:
        return None, ""
    except (OSError, ValueError) as error:
        return None, f"无法读取已保存统计：{error}"


def format_count(value) -> str:
    return "—" if value is None else f"{int(value):,}"


def format_duration(milliseconds) -> str:
    if milliseconds is None:
        return "—"
    minutes = max(0, int(milliseconds)) // 60000
    if minutes >= 1440:
        return f"{minutes // 1440} 天 {minutes % 1440 // 60} 小时"
    if minutes >= 60:
        return f"{minutes // 60} 小时 {minutes % 60} 分"
    return f"{minutes} 分钟"


def format_time(value: str, short=False) -> str:
    try:
        stamp = datetime.fromisoformat(value.replace("Z", "+00:00")).astimezone()
        return stamp.strftime("%m-%d %H:%M:%S" if short else "%Y-%m-%d %H:%M:%S")
    except (ValueError, AttributeError):
        return "未知"


def recent_result(row: dict) -> str:
    return "cancelled" if row.get("cancelled") else "success" if row.get("success") else "failure"


def seven_day_values(statistics: dict, now: datetime | None = None) -> list[tuple[str, int]]:
    today = (now or datetime.now().astimezone()).astimezone().date()
    days = {row.get("date"): row.get("totalTokens", 0) for row in statistics.get("daily", [])}
    return [((day := today - timedelta(days=6-i)).strftime("%m/%d"), int(days.get(day.isoformat(), 0)))
            for i in range(7)]
