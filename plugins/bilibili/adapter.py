#!/usr/bin/env python3
"""Bilibili command adapter. Importable logic also runs on Python 3.9.

Production network dependencies require Python 3.10+. Mutable state is kept
outside the capability package so its catalog digest remains stable.
"""
import sys
sys.dont_write_bytecode = True

import argparse
import asyncio
import base64
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import tempfile

if __package__:
    from .backend import BilibiliBackend, BilibiliError, translate_error
else:
    from backend import BilibiliBackend, BilibiliError, translate_error


AdapterError = BilibiliError
PACKAGE = Path(__file__).resolve().parent
MAX_INPUT_BYTES = 64 * 1024
MAX_DATA_UNITS = 3800
MAX_SEGMENT_CHARS = 800
MAX_PAGE_SEGMENTS = 12
BVID_PATTERN = re.compile(r"BV[1-9A-HJ-NP-Za-km-z]{10}\Z")
OPERATIONS = frozenset({"status", "search", "video_info", "comments", "read_transcript",
                        "start_transcription", "transcription_status"})
SOURCES = frozenset({"platform_subtitle", "local_asr"})


def json_text(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), allow_nan=False)


def utf16_size(value):
    return len(value.encode("utf-16-le")) // 2


def digest(value):
    return hashlib.sha256(json_text(value).encode("utf-8")).hexdigest()


def absolute_path(value, label, optional=False, preserve_symlinks=False):
    if value is None and optional:
        return None
    if not isinstance(value, str) or not value or "\x00" in value or not Path(value).is_absolute():
        raise AdapterError("INVALID_SETTINGS", label + " must be an absolute path")
    # Resolving a venv's python symlink selects its base interpreter and loses
    # the installed environment. Directory identities may still be resolved.
    return os.path.abspath(value) if preserve_symlinks else str(Path(value).resolve())


def validate_settings(value):
    allowed = {"stateDir", "credentialPath", "requestTimeoutSeconds", "asrEnabled", "asrPython",
               "asrModelPath", "asrMaxDurationSeconds", "asrJobTimeoutSeconds"}
    if not isinstance(value, dict) or set(value) - allowed:
        raise AdapterError("INVALID_SETTINGS", "Unknown or invalid plugin settings")
    state = absolute_path(value.get("stateDir"), "stateDir")
    if Path(state) == PACKAGE or PACKAGE in Path(state).parents:
        raise AdapterError("INVALID_SETTINGS", "stateDir must be outside the plugin package")
    result = {"stateDir": state,
              "credentialPath": absolute_path(value.get("credentialPath", str(Path(state) / "credential.json")), "credentialPath"),
              "asrPython": absolute_path(value.get("asrPython"), "asrPython", optional=True, preserve_symlinks=True),
              "asrModelPath": absolute_path(value.get("asrModelPath"), "asrModelPath", optional=True)}
    for key, default, low, high in (("requestTimeoutSeconds", 15, 1, 25),
                                  ("asrMaxDurationSeconds", 1800, 1, 14400),
                                  ("asrJobTimeoutSeconds", 900, 1, 7200)):
        item = value.get(key, default)
        if type(item) is not int or not low <= item <= high:
            raise AdapterError("INVALID_SETTINGS", key + " must be an integer from %d to %d" % (low, high))
        result[key] = item
    enabled = value.get("asrEnabled", False)
    if type(enabled) is not bool:
        raise AdapterError("INVALID_SETTINGS", "asrEnabled must be boolean")
    result["asrEnabled"] = enabled
    return result


def load_settings(path):
    absolute_path(path, "settings")
    try:
        with open(path, "rb") as source:
            data = source.read(MAX_INPUT_BYTES + 1)
        if len(data) > MAX_INPUT_BYTES:
            raise ValueError("Too large")
        return validate_settings(decode_json(data))
    except (OSError, ValueError, UnicodeError):
        raise AdapterError("INVALID_SETTINGS", "Plugin settings cannot be read as UTF-8 JSON") from None


def validate_arguments(operation, arguments):
    allowed = {"status": set(), "search": {"keyword", "page", "limit", "order"},
               "video_info": {"bvid", "part"}, "comments": {"bvid", "page", "limit"},
               "read_transcript": {"bvid", "part", "cursor", "source"},
               "start_transcription": {"bvid", "part"}, "transcription_status": {"job_id"}}
    if operation not in OPERATIONS:
        raise AdapterError("UNKNOWN_OPERATION", "Unknown Bilibili operation")
    required = set() if operation == "status" else {"keyword" if operation == "search" else
                                                    "job_id" if operation == "transcription_status" else "bvid"}
    if not isinstance(arguments, dict) or set(arguments) - allowed[operation] or required - set(arguments):
        raise AdapterError("INVALID_INPUT", "Missing or unexpected operation parameters")
    result = dict(arguments)
    if "job_id" in result and (not isinstance(result["job_id"], str) or not re.fullmatch(r"[a-f0-9]{32}", result["job_id"])):
        raise AdapterError("INVALID_INPUT", "job_id must come from a real transcription receipt")
    if "bvid" in result and (not isinstance(result["bvid"], str) or not BVID_PATTERN.fullmatch(result["bvid"])):
        raise AdapterError("INVALID_INPUT", "bvid must be a BV video identifier from a real result")
    if "keyword" in result:
        keyword = result["keyword"]
        if not isinstance(keyword, str) or not keyword.strip() or len(keyword) > 100 or any(ord(c) < 32 or 0xD800 <= ord(c) <= 0xDFFF for c in keyword):
            raise AdapterError("INVALID_INPUT", "keyword must contain 1 to 100 printable characters")
    for key, default, maximum in (("page", 1, 1000), ("part", 1, 1000),
                                  ("limit", 3 if operation == "comments" else 5, 5)):
        if key in allowed[operation]:
            item = result.get(key, default)
            if type(item) is not int or not 1 <= item <= maximum:
                raise AdapterError("INVALID_INPUT", key + " must be an integer from 1 to %d" % maximum)
            result[key] = item
    if operation == "search":
        if result.get("order", "totalrank") not in ("totalrank", "click", "pubdate", "dm"):
            raise AdapterError("INVALID_INPUT", "Unknown search order")
        result.setdefault("order", "totalrank")
    if operation == "read_transcript":
        if not isinstance(result.get("source", "platform_subtitle"), str) or result.get("source", "platform_subtitle") not in SOURCES:
            raise AdapterError("INVALID_INPUT", "Unknown transcript source")
        result.setdefault("source", "platform_subtitle")
        if "cursor" in result and (not isinstance(result["cursor"], str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,256}", result["cursor"])):
            raise AdapterError("INVALID_CURSOR", "Invalid transcript cursor; read the first page again")
    return result


def envelope(state, data=None, error=None):
    result = dict(data or {})
    if error is not None:
        result["error"] = {"code": error.code, "message": error.message}
    return {"state": state, "data": result}


def clipped_text(value, budget):
    """Bound serialized UTF-16 as well as Unicode length, preserving emoji."""
    if not isinstance(value, str):
        raise AdapterError("BACKEND_ERROR", "Bilibili returned an invalid text field")
    if utf16_size(json_text(value)) <= budget:
        return value, False
    low, high = 0, min(len(value), budget)
    while low < high:
        middle = (low + high + 1) // 2
        if utf16_size(json_text(value[:middle] + "…")) <= budget:
            low = middle
        else:
            high = middle - 1
    return value[:low] + "…", True


def video_url(bvid, part=1):
    return "https://www.bilibili.com/video/" + bvid + ("?p=" + str(part) if part != 1 else "")


def bounded_fields(value, fields):
    result = {}
    truncated = False
    for key, budget in fields:
        result[key], cut = clipped_text(value.get(key, ""), budget)
        truncated = truncated or cut
    return result, truncated


def public_video(info):
    result, truncated = bounded_fields(info, (("title", 180), ("partTitle", 180),
                                             ("author", 80), ("description", 1500)))
    for key in ("bvid", "aid", "cid", "part", "partCount", "durationSeconds"):
        result[key] = info[key]
    result.update(source="bilibili_video", url=video_url(info["bvid"], info["part"]), truncated=truncated)
    return result


def credential_identity(settings):
    """Only the content fingerprint participates in cache and job identities."""
    try:
        data = Path(settings["credentialPath"]).read_bytes()
    except FileNotFoundError:
        data = b""
    except OSError:
        raise AdapterError("INVALID_CREDENTIAL", "Credential file cannot be read") from None
    return hashlib.sha256(data).hexdigest()


def atomic_json(path, value):
    path = Path(path)
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        descriptor, temporary = tempfile.mkstemp(prefix=".pending-", dir=str(path.parent))
        try:
            with os.fdopen(descriptor, "w", encoding="utf-8") as output:
                output.write(json_text(value))
            os.replace(temporary, path)
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)
    except OSError:
        raise AdapterError("STATE_UNAVAILABLE", "Plugin state cannot be written") from None


def transcript_identity(settings, bvid, part, source):
    return digest([credential_identity(settings), bvid, part, source])


def normalize_segments(segments):
    result = []
    for segment in segments:
        start, end, text = segment["startMs"], segment["endMs"], segment["text"]
        if (type(start) is not int or type(end) is not int or start < 0 or end < start
                or not isinstance(text, str)):
            raise AdapterError("BACKEND_ERROR", "Transcript contains an invalid timed segment")
        # Keep the original interval; splitting text does not invent word timing.
        offset = 0
        while offset < len(text):
            size = min(MAX_SEGMENT_CHARS, len(text) - offset)
            # JSON escaping can expand a character to six units. Split, never
            # truncate, when an unusual source would overflow a tool string.
            while utf16_size(json_text(text[offset:offset + size])) > 1800:
                size //= 2
            result.append({"startMs": start, "endMs": end, "text": text[offset:offset + size]})
            offset += size
    return result


def save_transcript(settings, video, source, segments, language="", reason="", expected_account=None):
    """Publish timed text and its latest pointer. Returns immutable snapshot ID.

    Shared with the ASR worker; call only after the complete result exists.
    An empty local_asr result is a completed no_speech observation.
    """
    if source not in SOURCES:
        raise AdapterError("INVALID_INPUT", "Unknown transcript source")
    normalized = normalize_segments(segments)
    account = credential_identity(settings)
    if expected_account is not None and account != expected_account:
        raise AdapterError("CREDENTIAL_CHANGED", "The login changed before the transcript could be published")
    identity = digest([account, video["bvid"], video["part"], source])
    value = {"identity": identity, "bvid": video["bvid"], "part": video["part"], "cid": video["cid"],
             "title": clipped_text(video.get("title", ""), 180)[0], "source": source,
             "language": clipped_text(language, 80)[0], "segments": normalized,
             "availability": "available" if normalized else "unavailable",
             "reason": reason if not normalized and reason else ("" if normalized else "no_speech" if source == "local_asr" else "no_subtitle")}
    snapshot = digest(value)
    directory = Path(settings["stateDir"]) / "transcripts"
    path = directory / (snapshot + ".json")
    # A fresh first-page fetch also repairs a deleted or corrupt local file.
    # The content-derived name keeps repeated identical reads at one snapshot.
    atomic_json(path, value)
    atomic_json(directory / ("index-" + identity + ".json"), {"snapshot": snapshot})
    return snapshot


def make_cursor(snapshot, offset, identity):
    payload = [snapshot, offset, digest([snapshot, offset, identity])[:16]]
    return base64.urlsafe_b64encode(json_text(payload).encode("ascii")).decode("ascii").rstrip("=")


def read_transcript_page(settings, bvid, part, source, cursor=None):
    """Read cached timed text, returning data or None when no latest snapshot exists."""
    identity = transcript_identity(settings, bvid, part, source)
    directory = Path(settings["stateDir"]) / "transcripts"
    try:
        if cursor is not None:
            snapshot, offset, signature = json.loads(base64.urlsafe_b64decode(cursor + "=" * (-len(cursor) % 4)))
            if (type(offset) is not int or offset < 0 or signature != digest([snapshot, offset, identity])[:16]):
                raise ValueError("Invalid cursor")
        else:
            index = directory / ("index-" + identity + ".json")
            if not index.exists():
                return None
            snapshot = json.loads(index.read_text(encoding="utf-8"))["snapshot"]
            offset = 0
        if not isinstance(snapshot, str) or not re.fullmatch(r"[a-f0-9]{64}", snapshot):
            raise ValueError("Invalid snapshot")
        value = json.loads((directory / (snapshot + ".json")).read_text(encoding="utf-8"))
        if value["identity"] != identity or digest(value) != snapshot or offset > len(value["segments"]):
            raise ValueError("Snapshot changed")
        if cursor is not None and offset == len(value["segments"]):
            raise ValueError("Cursor is past the final page")
    except (OSError, ValueError, TypeError, KeyError):
        raise AdapterError("INVALID_CURSOR", "Transcript snapshot is unavailable or changed; read the first page again") from None
    data = {key: value[key] for key in ("bvid", "part", "cid", "title", "source", "language", "availability", "reason")}
    data.update(snapshotId=snapshot, url=video_url(bvid, part), totalSegments=len(value["segments"]),
                segments=[], hasMore=False, nextCursor=None)
    position = offset
    while position < len(value["segments"]) and len(data["segments"]) < MAX_PAGE_SEGMENTS:
        segment = value["segments"][position]
        data["segments"].append(segment)
        data["hasMore"] = position + 1 < len(value["segments"])
        data["nextCursor"] = make_cursor(snapshot, position + 1, identity) if data["hasMore"] else None
        if utf16_size(json_text(data)) > MAX_DATA_UNITS:
            data["segments"].pop()
            break
        position += 1
    if position == offset and value["segments"]:
        raise AdapterError("CONTENT_TOO_LARGE", "A transcript segment exceeds the page budget")
    data["hasMore"] = position < len(value["segments"])
    data["nextCursor"] = make_cursor(snapshot, position, identity) if data["hasMore"] else None
    return data


async def _execute(operation, arguments, settings, backend):
    if operation == "search":
        value = await backend.search(arguments["keyword"], arguments["page"], arguments["limit"], arguments["order"])
        entries, truncated = [], False
        for item in value["entries"]:
            entry, cut = bounded_fields(item, (("title", 120), ("author", 60), ("description", 160), ("duration", 24)))
            if not BVID_PATTERN.fullmatch(item["bvid"]):
                raise AdapterError("BACKEND_ERROR", "Search returned an invalid video identifier")
            entry.update(bvid=item["bvid"], url=video_url(item["bvid"]), truncated=cut)
            entries.append(entry)
            truncated = truncated or cut
        return {"source": "bilibili_search", "keyword": arguments["keyword"], "page": arguments["page"],
                "entries": entries, "total": value["total"], "hasMore": value["hasMore"], "truncated": truncated}
    if operation == "video_info":
        return public_video(await backend.video_info(arguments["bvid"], arguments["part"]))
    if operation == "comments":
        value = await backend.comments(arguments["bvid"], arguments["page"], arguments["limit"])
        entries, truncated = [], value.get("truncated", False)
        for item in value["entries"]:
            entry, cut = bounded_fields(item, (("author", 60), ("text", 400)))
            entry.update({key: item[key] for key in ("id", "userId", "likes", "time")})
            entry["truncated"] = cut
            entries.append(entry)
            truncated = truncated or cut
        return {"source": "bilibili_comments", "bvid": arguments["bvid"], "url": video_url(arguments["bvid"]),
                "page": arguments["page"], "entries": entries, "total": value["total"],
                "hasMore": value["hasMore"], "truncated": truncated}
    if operation == "read_transcript":
        bvid, part, source = arguments["bvid"], arguments["part"], arguments["source"]
        cursor = arguments.get("cursor")
        if not cursor and source == "platform_subtitle":
            account = credential_identity(settings)
            value = await backend.transcript(bvid, part)
            save_transcript(settings, value["video"], source, value["segments"],
                            language=value.get("language", ""), reason=value.get("reason", ""),
                            expected_account=account)
        data = read_transcript_page(settings, bvid, part, source, cursor)
        if data is None:
            return {"bvid": bvid, "part": part, "source": source, "url": video_url(bvid, part),
                    "availability": "unavailable", "reason": "transcription_not_ready", "segments": [],
                    "hasMore": False, "nextCursor": None}
        return data
    raise AdapterError("UNKNOWN_OPERATION", "Unknown Bilibili operation")


def status(settings):
    missing = [name for module, name in (("bilibili_api", "bilibili-api-python"), ("aiohttp", "aiohttp"))
               if importlib.util.find_spec(module) is None]
    return {"availability": "unavailable" if missing else "ready", "missingDependencies": missing,
            "credentialPresent": Path(settings["credentialPath"]).is_file(),
            "loginVerified": False, "asrEnabled": settings["asrEnabled"],
            "asrModelPresent": bool(settings["asrModelPath"] and Path(settings["asrModelPath"]).is_dir()),
            "asrPythonPresent": bool(settings["asrPython"] and Path(settings["asrPython"]).is_file())}


def run(operation, arguments, settings, backend=None):
    """Execute one bounded operation; backend injection is for code-level tests."""
    try:
        settings = validate_settings(settings)
        arguments = validate_arguments(operation, arguments)
        if operation in ("start_transcription", "transcription_status"):
            if __package__:
                from .transcription import start_transcription, transcription_status
            else:
                from transcription import start_transcription, transcription_status
            if operation == "start_transcription":
                return start_transcription(arguments, settings, backend)
            return transcription_status(arguments, settings)
        if operation == "status":
            data = status(settings)
        else:
            selected = backend if backend is not None else BilibiliBackend(settings)
            async def execute():
                return await asyncio.wait_for(_execute(operation, arguments, settings, selected),
                                              settings["requestTimeoutSeconds"])
            data = asyncio.run(execute())
        if utf16_size(json_text(data)) > MAX_DATA_UNITS:
            raise AdapterError("CONTENT_TOO_LARGE", "The result exceeds the tool output budget")
        return envelope("completed", data)
    except AdapterError as error:
        return envelope("failed", error=error)
    except Exception as error:
        return envelope("failed", error=translate_error(error))


def decode_json(raw):
    def reject_constant(_):
        raise ValueError("Invalid JSON number")
    return json.loads(raw.decode("utf-8"), parse_constant=reject_constant)


def main(argv=None, backend=None):
    for stream in (sys.stdin, sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--settings", required=True)
    parser.add_argument("--operation", required=True)
    options = parser.parse_args(argv)
    try:
        settings = load_settings(options.settings)
        raw = sys.stdin.buffer.read(MAX_INPUT_BYTES + 1)
        if len(raw) > MAX_INPUT_BYTES:
            raise AdapterError("INVALID_INPUT", "Input exceeds the JSON request limit")
        try:
            arguments = decode_json(raw)
        except (ValueError, UnicodeError):
            raise AdapterError("INVALID_INPUT", "Expected UTF-8 JSON operation arguments") from None
        result = run(options.operation, arguments, settings, backend)
    except AdapterError as error:
        result = envelope("failed", error=error)
    sys.stdout.write(json_text(result) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
