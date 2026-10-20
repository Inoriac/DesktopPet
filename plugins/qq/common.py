"""Settings and message conversion shared by the QQ command and source."""
import sys
sys.dont_write_bytecode = True

from datetime import datetime, timezone
import json
from pathlib import Path
import re
from urllib.parse import urlsplit

PACKAGE = Path(__file__).resolve().parent
MAX_JSON_BYTES = 64 * 1024
MAX_TEXT_UNITS = 8000
ID_PATTERN = re.compile(r"[1-9][0-9]{0,19}\Z")
CHAT_PATTERN = re.compile(r"(group|private):([1-9][0-9]{0,19})\Z")


class AdapterError(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code
        self.message = message


def json_text(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), allow_nan=False)


def decode_json(raw):
    def reject_constant(_):
        raise ValueError("Non-finite JSON value")
    return json.loads(raw, parse_constant=reject_constant)


def failed(error):
    return {"state": "failed", "data": {"error": {"code": error.code, "message": error.message}}}


def package_external_path(value, label):
    if not isinstance(value, str) or not value or "\x00" in value or not Path(value).is_absolute():
        raise AdapterError("INVALID_SETTINGS", label + " must be an absolute path")
    path = Path(value).resolve()
    if path == PACKAGE or PACKAGE in path.parents:
        raise AdapterError("INVALID_SETTINGS", label + " must be outside the plugin package")
    return str(path)


def validate_settings(value):
    allowed = {"backend", "wsUrl", "tokenFile", "selfId", "connectionKey", "stateDir",
               "allowedConversations", "requestTimeoutSeconds"}
    if not isinstance(value, dict) or set(value) - allowed:
        raise AdapterError("INVALID_SETTINGS", "Invalid or unknown QQ settings")
    result = dict(value)
    if result.get("backend") not in ("napcat", "snowluma"):
        raise AdapterError("INVALID_SETTINGS", "backend must be napcat or snowluma")
    if not isinstance(result.get("wsUrl"), str):
        raise AdapterError("INVALID_SETTINGS", "wsUrl must be a WebSocket URL string")
    try:
        url = urlsplit(result.get("wsUrl", ""))
        if url.scheme not in ("ws", "wss") or not url.hostname or url.username or url.password or url.query or url.fragment:
            raise ValueError()
        url.port
    except (TypeError, ValueError):
        raise AdapterError("INVALID_SETTINGS", "wsUrl must be a WebSocket URL without embedded credentials or query") from None
    if not isinstance(result.get("selfId"), str) or not ID_PATTERN.fullmatch(result["selfId"]):
        raise AdapterError("INVALID_SETTINGS", "selfId must be a positive QQ account identifier")
    key = result.get("connectionKey")
    if not isinstance(key, str) or not re.fullmatch(r"[A-Za-z0-9._-]{1,100}", key):
        raise AdapterError("INVALID_SETTINGS", "connectionKey must contain 1 to 100 letters, digits, dots, underscores or hyphens")
    rooms = result.get("allowedConversations")
    if not isinstance(rooms, list) or not 1 <= len(rooms) <= 100 or any(
            not isinstance(room, str) or not CHAT_PATTERN.fullmatch(room) for room in rooms) or len(set(rooms)) != len(rooms):
        raise AdapterError("INVALID_SETTINGS", "allowedConversations must contain 1 to 100 distinct group:<id> or private:<id> keys")
    if "private:" + result["selfId"] in rooms:
        raise AdapterError("INVALID_SETTINGS", "A private conversation must identify the other account")
    result["stateDir"] = package_external_path(result.get("stateDir"), "stateDir")
    if result.get("tokenFile") is not None:
        result["tokenFile"] = package_external_path(result["tokenFile"], "tokenFile")
    timeout = result.get("requestTimeoutSeconds", 10)
    if type(timeout) is not int or not 1 <= timeout <= 25:
        raise AdapterError("INVALID_SETTINGS", "requestTimeoutSeconds must be an integer from 1 to 25")
    result["requestTimeoutSeconds"] = timeout
    return result


def load_settings(path):
    path = package_external_path(path, "settings")
    try:
        with open(path, "rb") as source:
            raw = source.read(MAX_JSON_BYTES + 1)
        if len(raw) > MAX_JSON_BYTES:
            raise ValueError()
        return validate_settings(decode_json(raw.decode("utf-8")))
    except (OSError, ValueError, UnicodeError):
        raise AdapterError("INVALID_SETTINGS", "QQ settings must be readable UTF-8 JSON") from None


def authorize_chat(settings, chat_id):
    if not isinstance(chat_id, str) or chat_id not in settings["allowedConversations"]:
        raise AdapterError("PERMISSION_DENIED", "Conversation is not configured for this QQ connection")
    return chat_id.split(":", 1)


def remote_id(value):
    if type(value) is int:
        value = str(value)
    if not isinstance(value, str) or not value or len(value) > 128 or any(ord(c) < 32 or 0xD800 <= ord(c) <= 0xDFFF for c in value):
        return ""
    return value


def qq_id(value):
    value = remote_id(value)
    return value if ID_PATTERN.fullmatch(value) else ""


def bounded_text(value, limit=MAX_TEXT_UNITS):
    # Lone surrogates are invalid Unicode; replace before emitting UTF-8 JSON.
    value = value.encode("utf-16-le", errors="replace").decode("utf-16-le")
    raw = value.encode("utf-16-le")
    if len(raw) <= limit * 2:
        return value
    marker = "…[内容已截断]"
    return raw[:(limit - len(marker)) * 2].decode("utf-16-le", errors="ignore") + marker


def timestamp(value):
    if type(value) not in (int, float) or value <= 0:
        return None
    try:
        return datetime.fromtimestamp(value, timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")
    except (ValueError, OverflowError, OSError):
        return None


def unescape_cq(value):
    return value.replace("&#91;", "[").replace("&#93;", "]").replace("&#44;", ",").replace("&amp;", "&")


def segments(value):
    if isinstance(value, list):
        return value
    if not isinstance(value, str):
        return []
    output, end = [], 0
    for match in re.finditer(r"\[CQ:([A-Za-z_]+)((?:,[^\]]*)?)\]", value):
        output.append({"type": "text", "data": {"text": unescape_cq(value[end:match.start()])}})
        data = {}
        for parameter in match.group(2).lstrip(",").split(","):
            key, separator, content = parameter.partition("=")
            if separator:
                data[key] = unescape_cq(content)
        output.append({"type": match.group(1), "data": data})
        end = match.end()
    output.append({"type": "text", "data": {"text": unescape_cq(value[end:])}})
    return output


def render_message(value):
    parts, reply_to = [], ""
    placeholders = {"image": "[图片：尚未解析]", "record": "[语音：尚未转写]", "video": "[视频：尚未解析]",
                    "file": "[文件]", "forward": "[合并转发：尚未展开]", "json": "[分享卡片]", "xml": "[分享卡片]"}
    for segment in segments(value):
        if not isinstance(segment, dict) or not isinstance(segment.get("data"), dict):
            continue
        kind, data = segment.get("type"), segment["data"]
        if not isinstance(kind, str):
            continue
        if kind == "text" and isinstance(data.get("text"), str):
            parts.append(data["text"])
        elif kind == "at":
            parts.append("@所有人" if data.get("qq") == "all" else "@" + (qq_id(data.get("qq")) or "未知账号"))
        elif kind == "reply":
            reply_to = reply_to or remote_id(data.get("id"))
        elif kind == "face":
            parts.append("[表情:" + (remote_id(data.get("id")) or "未知") + "]")
        else:
            parts.append(placeholders.get(kind, "[不支持的消息类型]"))
    text = bounded_text("".join(parts))
    if not text.strip() and reply_to:
        text = "[引用消息]"
    return text, reply_to


def normalize_event(settings, event, historical=False, requested_chat=None):
    if not isinstance(event, dict) or event.get("post_type", "message") not in ("message", "message_sent"):
        return None
    if "self_id" in event and qq_id(event["self_id"]) != settings["selfId"]:
        return None
    event = dict(event)
    requested_kind, requested_id = requested_chat.split(":") if requested_chat else (None, None)
    # History responses may omit routing metadata. Fill only missing fields,
    # and check explicit metadata before trusting the requested conversation.
    kind = event.get("message_type", requested_kind)
    if kind not in ("group", "private") or requested_kind and kind != requested_kind:
        return None
    sender = event.get("sender") if isinstance(event.get("sender"), dict) else {}
    actor = qq_id(sender.get("user_id")) or qq_id(event.get("user_id"))
    if kind == "group":
        target = qq_id(event.get("group_id")) if "group_id" in event else requested_id
    else:
        # A temporary private message can carry group_id; it is still private.
        target = actor if actor and actor != settings["selfId"] else qq_id(event.get("target_id"))
        if not target:
            target = requested_id
        if requested_id and qq_id(event.get("target_id")) not in ("", settings["selfId"], requested_id):
            return None
    if not target or requested_id and target != requested_id:
        return None
    chat_id = kind + ":" + target
    if chat_id not in settings["allowedConversations"]:
        return None
    identifier = remote_id(event.get("message_id"))
    if not identifier:
        return None
    text, reply = render_message(event.get("message", event.get("raw_message")))
    if not text.strip():
        return None
    display = sender.get("card") or sender.get("nickname") or ""
    item = {"chat_id": chat_id, "message_id": identifier, "speaker_id": actor,
            "display_name": bounded_text(display, 256) if isinstance(display, str) else "",
            "text": text, "reply_to": reply, "historical": bool(historical)}
    occurred = timestamp(event.get("time"))
    if occurred:
        item["occurred_at"] = occurred
    return item


def observation(settings, item):
    result = {"type": "observation", "kind": "message", "eventId": item["event_id"],
              "sequence": item["sequence"], "cursor": item["cursor"],
              "conversation": {"remoteConversationId": item["chat_id"], "kind": "group" if item["chat_id"].startswith("group:") else "direct"},
              "speaker": {"provider": "qq", "namespace": settings["connectionKey"],
                          "remoteActorId": item["speaker_id"], "displayName": item["display_name"]},
              "message": {"remoteMessageId": item["message_id"], "text": item["text"], "replyToRemoteId": item["reply_to"]},
              "historicalImport": item["historical"]}
    if "occurred_at" in item:
        result["occurredAt"] = item["occurred_at"]
    return result
