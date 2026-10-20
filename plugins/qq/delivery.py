"""Text-only OneBot sends with durable, query-only delivery receipts."""
import sys
sys.dont_write_bytecode = True

import sqlite3

if __package__:
    from .common import AdapterError, MAX_TEXT_UNITS, authorize_chat, failed, qq_id, remote_id, render_message, timestamp, validate_settings
    from .journal import Journal
    from .transport import OneBotClient
else:
    from common import AdapterError, MAX_TEXT_UNITS, authorize_chat, failed, qq_id, remote_id, render_message, timestamp, validate_settings
    from journal import Journal
    from transport import OneBotClient


def validate_arguments(settings, arguments, operation):
    fields = {"chat_id", "text", "reply_to"} if operation == "send" else {"chat_id", "receipt_id"}
    if not isinstance(arguments, dict) or set(arguments) - fields:
        raise AdapterError("INVALID_INPUT", "Invalid operation arguments")
    authorize_chat(settings, arguments.get("chat_id"))
    if operation == "receipt":
        if not isinstance(arguments.get("receipt_id"), str) or not remote_id(arguments["receipt_id"]):
            raise AdapterError("INVALID_INPUT", "receipt_id must identify an earlier send attempt")
        return
    text = arguments.get("text")
    if not isinstance(text, str) or not text.strip():
        raise AdapterError("INVALID_INPUT", "text must be a nonempty string")
    try:
        length = len(text.encode("utf-16-le")) // 2
    except UnicodeError:
        raise AdapterError("INVALID_INPUT", "text must contain valid Unicode") from None
    if length > MAX_TEXT_UNITS:
        raise AdapterError("INVALID_INPUT", "text must not exceed 8000 UTF-16 units")
    reply_to = arguments.get("reply_to", "")
    if not isinstance(reply_to, str) or reply_to and not remote_id(reply_to):
        raise AdapterError("INVALID_INPUT", "reply_to must be a message identifier from this conversation")


def result_for(attempt, status="unknown", confirmed=None, error=None):
    data = {"receipt_id": attempt["receipt_id"], "delivery": status}
    if attempt["remote_id"]:
        data["remote_message_id"] = attempt["remote_id"]
    if confirmed:
        data.update(confirmed)
    if error:
        data["error"] = {"code": error.code, "message": error.message}
    if status == "unknown":
        data["warning"] = "Delivery is unconfirmed. Query this receipt; do not send the message again."
    return {"state": status, "data": data, "receipt": attempt["receipt_id"],
            "tracking": {"operation": "receipt", "arguments": {"chat_id": attempt["chat_id"], "receipt_id": attempt["receipt_id"]}}}


def confirmed_message(settings, attempt, value, from_private_history=False):
    """Return actual fields and whether the response identifies the peer."""
    if not isinstance(value, dict) or remote_id(value.get("message_id")) != attempt["remote_id"]:
        return None
    kind, target = attempt["chat_id"].split(":", 1)
    message_type = value.get("message_type", "private" if from_private_history else None)
    if message_type != kind or "self_id" in value and qq_id(value["self_id"]) != settings["selfId"]:
        return None
    sender = value.get("sender") if isinstance(value.get("sender"), dict) else {}
    actor = qq_id(sender.get("user_id")) or qq_id(value.get("user_id"))
    if actor != settings["selfId"]:
        return None
    if kind == "group":
        if qq_id(value.get("group_id")) != target:
            return None
        if settings["backend"] == "snowluma" and (type(value.get("message_seq")) is not int or value["message_seq"] <= 0):
            return None
        target_known = True
    else:
        peers = [value[key] for key in ("target_id", "peer_id") if key in value]
        if any(qq_id(peer) != target for peer in peers):
            return None
        target_known = bool(peers) or from_private_history
    occurred_at = timestamp(value.get("time"))
    content, _ = render_message(value.get("message", value.get("raw_message")))
    if not occurred_at or not content.strip():
        return None
    return {"remote_message_id": attempt["remote_id"], "content": content, "occurred_at": occurred_at}, target_known


def confirm(settings, client, attempt):
    value = client.call("get_msg", {"message_id": attempt["remote_id"]})
    match = confirmed_message(settings, attempt, value)
    if match is None:
        return result_for(attempt)
    actual, target_known = match
    if target_known:
        return result_for(attempt, "completed", actual)
    # NapCat's outgoing private get_msg carries sender=user_id and no peer.
    # Only a target-scoped history response may supply that missing scope.
    target = attempt["chat_id"].split(":", 1)[1]
    history = client.call("get_friend_msg_history", {"user_id": int(target), "count": 50})
    if isinstance(history, dict) and isinstance(history.get("messages"), list):
        for item in history["messages"]:
            match = confirmed_message(settings, attempt, item, from_private_history=True)
            if match:
                return result_for(attempt, "completed", match[0])
    return result_for(attempt)


def storage_error():
    return AdapterError("STORAGE_ERROR", "Cannot persist QQ delivery state; keep the receipt and do not resend")


def keep_unknown(settings, attempt, error):
    """Preserve the receipt even when a post-dispatch journal write failed."""
    result = result_for(attempt, error=error)
    try:
        with Journal(settings) as journal:
            journal.update_attempt(attempt, result)
    except (AdapterError, sqlite3.Error, OSError):
        # The returned receipt still identifies the committed pending row.
        # It cannot recover a remote ID that never reached durable storage.
        pass
    return result


def send(settings, arguments):
    attempt = None
    dispatched = False
    try:
        settings = validate_settings(settings)
        validate_arguments(settings, arguments, "send")
        chat_id, text = arguments["chat_id"], arguments["text"]
        reply_to = arguments.get("reply_to", "")
        with Journal(settings) as journal:
            if reply_to and not journal.has_message(chat_id, reply_to):
                raise AdapterError("UNKNOWN_REPLY", "reply_to must be an archived message in this conversation")
            with OneBotClient(settings) as client:
                attempt = journal.create_attempt(chat_id, text, reply_to)
                kind, target = chat_id.split(":", 1)
                message = ([{"type": "reply", "data": {"id": reply_to}}] if reply_to else [])
                message.append({"type": "text", "data": {"text": text}})
                params = {"group_id" if kind == "group" else "user_id": int(target), "message": message}
                action = "send_group_msg" if kind == "group" else "send_private_msg"
                if settings["backend"] == "snowluma":
                    action, params["message_type"] = "send_msg", kind
                dispatched = True
                try:
                    response = client.call(action, params)
                except AdapterError as error:
                    status = "failed" if error.code in ("API_REJECTED", "UNSUPPORTED_ACTION") else "unknown"
                    result = result_for(attempt, status, error=error)
                else:
                    attempt["remote_id"] = remote_id(response.get("message_id")) if isinstance(response, dict) else ""
                    # Save the known ID before any further network request.
                    journal.update_attempt(attempt, result_for(attempt))
                    result = confirm(settings, client, attempt) if attempt["remote_id"] else result_for(attempt)
                journal.update_attempt(attempt, result)
                return result
    except (AdapterError, sqlite3.Error, OSError) as error:
        error = error if isinstance(error, AdapterError) else storage_error()
        if dispatched:
            return keep_unknown(settings, attempt, error)
        return failed(error)


def receipt(settings, arguments):
    attempt = None
    try:
        settings = validate_settings(settings)
        validate_arguments(settings, arguments, "receipt")
        with Journal(settings) as journal:
            attempt = journal.get_attempt(arguments["chat_id"], arguments["receipt_id"])
            if attempt["status"] in ("completed", "failed"):
                result = attempt["result"]
            elif not attempt["remote_id"]:
                result = result_for(attempt)
            else:
                with OneBotClient(settings) as client:
                    result = confirm(settings, client, attempt)
                journal.update_attempt(attempt, result)
    except (AdapterError, sqlite3.Error, OSError) as error:
        error = error if isinstance(error, AdapterError) else storage_error()
        if attempt is None:
            return failed(error)
        result = keep_unknown(settings, attempt, error)
    # This operation completed the query; data.delivery is the send outcome.
    return dict(result, state="completed")
