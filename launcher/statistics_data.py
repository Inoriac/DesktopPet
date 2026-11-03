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


def _parse_iso_seconds(value: str) -> float | None:
    if not value or not isinstance(value, str):
        return None
    try:
        text = value.strip()
        if text.endswith("Z"):
            text = text[:-1] + "+00:00"
        dt = datetime.fromisoformat(text)
        if dt.tzinfo is None:
            dt = dt.astimezone()
        return dt.timestamp()
    except (ValueError, OSError):
        return None


def find_call_log_pair(profile_id: str, call: dict) -> dict | None:
    """Locate the request/response JSONL pair in ai_calls.jsonl for a call row."""
    try:
        logs_dir = profile_directory(profile_id) / "logs"
    except ValueError:
        return None
    if not logs_dir.is_dir():
        return None

    attempt_id = str(call.get("attemptId") or "")
    completed_ts = _parse_iso_seconds(str(call.get("completedAt") or ""))
    duration_sec = max(0.0, float(call.get("durationMs") or 0) / 1000.0)
    expected_req_ts = (completed_ts - duration_sec) if completed_ts is not None else None

    candidates = [logs_dir / "ai_calls.jsonl"]
    for idx in range(1, 4):
        backup = logs_dir / f"ai_calls.jsonl.{idx}"
        if backup.is_file():
            candidates.append(backup)

    requests_by_id: dict[str, dict] = {}
    responses_by_id: dict[str, dict] = {}
    for log_file in candidates:
        if not log_file.is_file():
            continue
        try:
            with log_file.open("r", encoding="utf-8", errors="replace") as handle:
                for line in handle:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        record = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    if not isinstance(record, dict):
                        continue
                    rid = str(record.get("request_id") or "")
                    if not rid:
                        continue
                    rtype = record.get("type")
                    if rtype == "request" and rid not in requests_by_id:
                        requests_by_id[rid] = record
                    elif rtype == "response" and rid not in responses_by_id:
                        responses_by_id[rid] = record
        except OSError:
            continue

    if attempt_id and (attempt_id in requests_by_id or attempt_id in responses_by_id):
        return {
            "requestId": attempt_id,
            "request": requests_by_id.get(attempt_id),
            "response": responses_by_id.get(attempt_id),
        }

    # Fallback for dialogue calls where requestId differs from attemptId:
    # match by closest timestamp within a 5-second window.
    best_rid = ""
    best_score = 10.0
    call_success = bool(call.get("success"))
    for rid, resp in responses_by_id.items():
        resp_ts = _parse_iso_seconds(str(resp.get("timestamp") or ""))
        if completed_ts is not None and resp_ts is not None:
            diff = abs(resp_ts - completed_ts)
            if diff <= 5.0:
                if bool(resp.get("success")) != call_success:
                    diff += 2.0
                if diff < best_score:
                    best_score = diff
                    best_rid = rid
    if not best_rid and expected_req_ts is not None:
        for rid, req in requests_by_id.items():
            req_ts = _parse_iso_seconds(str(req.get("timestamp") or ""))
            if req_ts is not None:
                diff = abs(req_ts - expected_req_ts)
                if diff <= 5.0 and diff < best_score:
                    best_score = diff
                    best_rid = rid

    if best_rid:
        return {
            "requestId": best_rid,
            "request": requests_by_id.get(best_rid),
            "response": responses_by_id.get(best_rid),
        }
    return None
