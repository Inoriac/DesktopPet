#!/usr/bin/env python3
"""QQ tool commands. Configure an external OneBot server; no UI automation."""
import sys
sys.dont_write_bytecode = True

import argparse
import hashlib
import sqlite3

if __package__:
    from .common import AdapterError, MAX_JSON_BYTES, authorize_chat, bounded_text, decode_json, failed, json_text, load_settings, timestamp, validate_settings
    from .journal import Journal
    from .transport import OneBotClient
    from .delivery import receipt, send
else:
    from common import AdapterError, MAX_JSON_BYTES, authorize_chat, bounded_text, decode_json, failed, json_text, load_settings, timestamp, validate_settings
    from journal import Journal
    from transport import OneBotClient
    from delivery import receipt, send


def execute(settings, operation, arguments):
    try:
        settings = validate_settings(settings)
        if operation in ("send", "receipt"):
            return (send if operation == "send" else receipt)(settings, arguments)
        allowed = {"status": set(), "list": {"cursor", "limit"}, "read": {"chat_id", "cursor", "limit"}}
        if operation not in allowed:
            raise AdapterError("UNKNOWN_OPERATION", "Unknown QQ operation")
        if not isinstance(arguments, dict) or set(arguments) - allowed[operation]:
            raise AdapterError("INVALID_INPUT", "Invalid operation arguments")
        if operation == "read":
            kind, target = authorize_chat(settings, arguments.get("chat_id"))
        if operation in ("read", "list"):
            if not isinstance(arguments.get("cursor", ""), str):
                raise AdapterError("INVALID_CURSOR", "cursor must be a string from a read result")
            if type(arguments.get("limit", 10)) is not int or not 1 <= arguments.get("limit", 10) <= 20:
                raise AdapterError("INVALID_INPUT", "limit must be an integer from 1 to 20")
        with Journal(settings) as journal:
            if operation == "list":
                allowed_rooms = settings["allowedConversations"]
                digest = hashlib.sha256(json_text(allowed_rooms).encode("utf-8")).hexdigest()[:16]
                prefix = "qq-list:" + digest + ":"
                cursor, offset = arguments.get("cursor", ""), 0
                if cursor:
                    suffix = cursor[len(prefix):] if cursor.startswith(prefix) else ""
                    if not suffix.isascii() or not suffix.isdigit() or len(suffix) > 3:
                        raise AdapterError("INVALID_CURSOR", "List cursor does not match the current allowed conversations")
                    offset = int(suffix)
                    if not 0 < offset < len(allowed_rooms) or str(offset) != suffix:
                        raise AdapterError("INVALID_CURSOR", "List cursor is outside the current allowed conversations")
                rooms, units = [], 0
                for chat_id in allowed_rooms[offset:offset + arguments.get("limit", 10)]:
                    last = journal.read(chat_id, limit=1)["items"]
                    summary = None
                    if last:
                        summary = {key: last[0][key] for key in ("message_id", "speaker_id", "occurred_at") if key in last[0]}
                        summary["text"] = bounded_text(last[0]["text"], 160)
                    room = {"chat_id": chat_id, "kind": "group" if chat_id.startswith("group:") else "direct",
                            "last_message": summary}
                    size = len(json_text(room).encode("utf-16-le")) // 2
                    if rooms and units + size > 3500:
                        break
                    rooms.append(room)
                    units += size
                offset += len(rooms)
                return {"state": "completed", "data": {"conversations": rooms,
                        "next_cursor": prefix + str(offset) if offset < len(allowed_rooms) else ""}}
            if operation == "read" and arguments.get("cursor"):
                data = journal.read(arguments["chat_id"], arguments["cursor"], arguments.get("limit", 10))
                data["freshness"] = "local_page"
                return {"state": "completed", "data": data}
            with OneBotClient(settings) as client:
                if operation == "status":
                    return {"state": "completed", "data": {"connected": True, "self_id": settings["selfId"], "backend": settings["backend"]}}
                warning = None
                try:
                    action = "get_group_msg_history" if kind == "group" else "get_friend_msg_history"
                    history = client.call(action, {"group_id" if kind == "group" else "user_id": int(target), "count": 50})
                    if not isinstance(history, dict) or not isinstance(history.get("messages"), list):
                        raise AdapterError("PROTOCOL_ERROR", "OneBot history must contain a messages array")
                    messages = [value for value in history["messages"] if isinstance(value, dict)]
                    messages.sort(key=lambda value: timestamp(value.get("time")) or "")
                    for value in messages[-50:]:
                        journal.ingest(value, historical=True, requested_chat=arguments["chat_id"])
                except AdapterError as error:
                    if error.code != "UNSUPPORTED_ACTION":
                        raise
                    warning = "The backend does not support this history API; only previously archived local messages are shown."
            data = journal.read(arguments["chat_id"], limit=arguments.get("limit", 10))
            data["freshness"] = "local_only" if warning else "remote"
            if warning:
                data["warning"] = warning
            return {"state": "completed", "data": data}
    except AdapterError as error:
        return failed(error)
    except (sqlite3.Error, OSError):
        return failed(AdapterError("STORAGE_ERROR", "Cannot access the QQ journal; existing state was not reset"))


def main(argv=None):
    for stream in (sys.stdin, sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--settings", required=True)
    parser.add_argument("--operation", required=True)
    options = parser.parse_args(argv)
    try:
        settings = load_settings(options.settings)
        raw = sys.stdin.buffer.read(MAX_JSON_BYTES + 1)
        if len(raw) > MAX_JSON_BYTES:
            raise AdapterError("INVALID_INPUT", "Operation arguments exceed 64 KiB")
        try:
            arguments = decode_json(raw.decode("utf-8"))
        except (ValueError, UnicodeError):
            raise AdapterError("INVALID_INPUT", "Operation arguments must be UTF-8 JSON") from None
        result = execute(settings, options.operation, arguments)
    except AdapterError as error:
        result = failed(error)
    sys.stdout.write(json_text(result) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
