#!/usr/bin/env python3
"""LX Music command capability. Python 3.9+, standard library only.

Host settings choose the endpoint, database and executable. Business input can
never choose paths or commands. Reads do not start LX; writes are dispatched at
most once and an unverified outcome remains unknown.
"""
import argparse
import http.client
import ipaddress
import json
import math
import os
from pathlib import Path
import platform
import socket
import sqlite3
import subprocess
import sys
import threading
import time
import unicodedata
from urllib.parse import parse_qs, quote, urlencode, urlsplit


MAX_INPUT_BYTES = 64 * 1024
MAX_HTTP_BYTES = 256 * 1024
MAX_OUTPUT_BYTES = 1024 * 1024
MAX_ROWS = 1000
MAX_FIELD_CHARS = 4096
READ_OPERATIONS = frozenset({"status", "lyric", "list_playlists", "playlist_songs"})
OPERATIONS = READ_OPERATIONS | frozenset({"launch", "play", "pause", "skip_next",
                                        "skip_prev", "set_volume", "search_play", "play_playlist"})


class AdapterError(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code
        self.message = message

    def as_dict(self):
        return {"code": self.code, "message": self.message}


def reject_json_constant(_value):
    raise ValueError("Non-finite JSON number")


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate JSON key")
        result[key] = value
    return result


def decode_json(raw, code="INVALID_INPUT"):
    try:
        return json.loads(raw.decode("utf-8"), parse_constant=reject_json_constant,
                          object_pairs_hook=unique_object)
    except (UnicodeError, ValueError, RecursionError):
        raise AdapterError(code, "Expected valid UTF-8 JSON") from None


def absolute_path(value, label, optional=False):
    if optional and value is None:
        return None
    if not isinstance(value, str) or not value or "\x00" in value or not Path(value).is_absolute():
        raise AdapterError("INVALID_SETTINGS", label + " must be an absolute path")
    return value


def validate_settings(raw):
    """Return normalized host settings without touching any configured resource."""
    allowed = {"apiUrl", "database", "executable", "requestTimeoutMs", "verifyTimeoutMs"}
    if not isinstance(raw, dict) or set(raw) - allowed:
        raise AdapterError("INVALID_SETTINGS", "Unknown or invalid connection settings")
    result = dict(raw)
    result["database"] = absolute_path(raw.get("database"), "database")
    result["executable"] = absolute_path(raw.get("executable"), "executable", optional=True)
    result["apiUrl"] = raw.get("apiUrl", "http://127.0.0.1:23330")
    value = result["apiUrl"]
    if not isinstance(value, str) or any(ord(character) <= 32 for character in value):
        raise AdapterError("INVALID_SETTINGS", "apiUrl must be an HTTP loopback origin")
    try:
        parsed = urlsplit(value)
        host = parsed.hostname
        loopback = host == "localhost" or bool(host and ipaddress.ip_address(host).is_loopback)
        port = parsed.port
        valid = (parsed.scheme == "http" and loopback and parsed.username is None
                 and parsed.password is None and parsed.path in ("", "/")
                 and not parsed.query and not parsed.fragment and "?" not in value and "#" not in value
                 and (port is None or 1 <= port <= 65535))
    except ValueError:
        valid = False
    if not valid:
        raise AdapterError("INVALID_SETTINGS", "apiUrl must be an HTTP loopback origin without credentials")
    result["apiUrl"] = value.rstrip("/")
    for key, default, minimum, maximum in (("requestTimeoutMs", 1000, 100, 5000),
                                           ("verifyTimeoutMs", 8000, 100, 15000)):
        value = raw.get(key, default)
        if type(value) is not int or not minimum <= value <= maximum:
            raise AdapterError("INVALID_SETTINGS", key + " is outside its allowed integer range")
        result[key] = value
    return result


def load_settings(path):
    absolute_path(path, "settings")
    try:
        with open(path, "rb") as source:
            raw = source.read(MAX_INPUT_BYTES + 1)
    except OSError:
        raise AdapterError("INVALID_SETTINGS", "Connection settings cannot be read") from None
    if len(raw) > MAX_INPUT_BYTES:
        raise AdapterError("INVALID_SETTINGS", "Connection settings exceed the size limit")
    return validate_settings(decode_json(raw, "INVALID_SETTINGS"))


def validate_arguments(operation, arguments):
    if operation not in OPERATIONS:
        raise AdapterError("UNKNOWN_OPERATION", "Unknown music operation")
    allowed = {"set_volume": {"volume"}, "search_play": {"song", "artist"},
               "playlist_songs": {"playlist_id"}, "play_playlist": {"playlist_id"}}.get(operation, set())
    required = allowed - {"artist"}
    if not isinstance(arguments, dict) or set(arguments) - allowed or required - set(arguments):
        raise AdapterError("INVALID_INPUT", "Missing or unexpected operation parameters")
    if operation == "set_volume":
        value = arguments["volume"]
        if type(value) is not int or not 0 <= value <= 100:
            raise AdapterError("INVALID_INPUT", "volume must be an integer from 0 to 100")
    for key in ("song", "artist", "playlist_id"):
        if key not in arguments:
            continue
        value = arguments[key]
        limit = 512 if key == "playlist_id" else 256
        if (not isinstance(value, str) or len(value) > limit or
                (key != "artist" and not value.strip()) or
                any(unicodedata.category(character) in ("Cc", "Cs") for character in value)):
            raise AdapterError("INVALID_INPUT", key + " must be a bounded, valid text value")
    return arguments


def envelope(state, data=None, error=None):
    data = dict(data or {})
    if error:
        data["error"] = error.as_dict()
    return {"state": state, "data": data}


class MusicAdapter:
    def __init__(self, settings):
        self.settings = settings
        self.dispatched = False
        self.evidence = {}

    def api(self, path, query=None, timeout=None, response_kind="json"):
        """One HTTP request, with no proxy, redirect, retry or implicit launch."""
        parsed = urlsplit(self.settings["apiUrl"])
        budget = self.settings["requestTimeoutMs"] / 1000.0
        if timeout is not None:
            budget = min(budget, timeout)
        if budget <= 0:
            raise AdapterError("API_TIMEOUT", "The API request time budget expired")
        # Resolve localhost to a fixed loopback address. No DNS or proxy lookup.
        host = "127.0.0.1" if parsed.hostname == "localhost" else parsed.hostname
        connection = http.client.HTTPConnection(host, parsed.port or 80, timeout=budget)
        deadline = time.monotonic() + budget
        target = path + ("?" + urlencode(query) if query else "")
        timer = None
        try:
            connection.connect()
            transport = connection.sock
            def expire():
                try:
                    transport.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
            # Socket timeouts alone allow a slow trickle of headers to keep a
            # request alive. This also caps the complete request's wall time.
            timer = threading.Timer(max(0, deadline - time.monotonic()), expire)
            timer.daemon = True
            timer.start()
            connection.request("GET", target, headers={"Accept": "application/json", "Connection": "close"})
            response = connection.getresponse()
            if not 200 <= response.status < 300:
                raise AdapterError("API_HTTP_ERROR", "The API returned HTTP %d" % response.status)
            length = response.getheader("Content-Length")
            if length is not None and (not length.isdecimal() or int(length) > MAX_HTTP_BYTES):
                raise AdapterError("API_RESPONSE_TOO_LARGE", "The API response size is invalid or too large")
            chunks = []
            size = 0
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise AdapterError("API_TIMEOUT", "The API request timed out")
                # HTTPResponse owns the socket after Connection: close headers.
                raw_socket = getattr(getattr(response.fp, "raw", None), "_sock", None)
                if raw_socket is not None:
                    raw_socket.settimeout(remaining)
                chunk = response.read1(min(65536, MAX_HTTP_BYTES + 1 - size))
                if not chunk:
                    break
                chunks.append(chunk)
                size += len(chunk)
                if size > MAX_HTTP_BYTES:
                    raise AdapterError("API_RESPONSE_TOO_LARGE", "The API response exceeds the size limit")
            raw = b"".join(chunks)
            if length is not None and len(raw) != int(length):
                raise AdapterError("API_INCOMPLETE_RESPONSE", "The API response ended before its declared length")
            if time.monotonic() >= deadline:
                raise AdapterError("API_TIMEOUT", "The API request timed out")
            content_type = response.getheader("Content-Type", "").split(";", 1)[0].strip().lower()
            if response_kind == "ack" and not raw:
                body = None
            elif response_kind == "json" or content_type == "application/json" or content_type.endswith("+json"):
                body = decode_json(raw, "API_INVALID_JSON")
            else:
                try:
                    body = raw.decode("utf-8")
                except UnicodeError:
                    raise AdapterError("API_INVALID_TEXT", "The API response is not UTF-8 text") from None
                if response_kind == "ack" and body.strip().casefold() in ("false", "error", "failed", "failure"):
                    raise AdapterError("API_REJECTED", "The API reported an error")
            if body is False or (isinstance(body, dict) and (body.get("ok") is False or body.get("success") is False or body.get("error"))):
                raise AdapterError("API_REJECTED", "The API reported an error")
            return body
        except (TimeoutError, socket.timeout):
            raise AdapterError("API_TIMEOUT", "The API request timed out") from None
        except (OSError, http.client.HTTPException, ValueError):
            raise AdapterError("API_UNAVAILABLE", "The local API did not return a complete response") from None
        finally:
            if timer is not None:
                timer.cancel()
            connection.close()

    def status(self, timeout=None):
        value = self.api("/status", timeout=timeout)
        if not isinstance(value, dict) or not isinstance(value.get("status"), str) or not value["status"].strip():
            raise AdapterError("API_INVALID_STATUS", "The API response has no playback status")
        self.evidence["status"] = value
        return value

    def database(self):
        path = Path(self.settings["database"])
        if not path.is_file():
            raise AdapterError("DATABASE_UNAVAILABLE", "The music database does not exist")
        try:
            connection = sqlite3.connect(path.as_uri() + "?mode=ro", uri=True,
                                         timeout=self.settings["requestTimeoutMs"] / 1000.0)
            connection.execute("PRAGMA query_only=ON")
            deadline = time.monotonic() + self.settings["requestTimeoutMs"] / 1000.0
            connection.set_progress_handler(lambda: int(time.monotonic() > deadline), 1000)
            if hasattr(connection, "setlimit"):
                connection.setlimit(sqlite3.SQLITE_LIMIT_LENGTH, MAX_OUTPUT_BYTES)
            return connection
        except sqlite3.Error:
            raise AdapterError("DATABASE_UNAVAILABLE", "The music database cannot be opened read-only") from None

    @staticmethod
    def playlist_url(source, source_id):
        if not source or not source_id:
            return None
        source = str(source).strip()
        identifier = str(source_id).strip()
        if not source or not identifier:
            return None
        parsed = urlsplit(identifier)
        if parsed.scheme in ("http", "https"):
            query = parse_qs(parsed.query)
            if source in ("wy", "tx") and query.get("id"):
                identifier = query["id"][0]
            elif query.get("playlistId"):
                identifier = query["playlistId"][0]
        # The existing LX adapter preserves path separators inside source IDs.
        # Keep those separators while encoding characters that could introduce
        # a query/fragment or break a single executable argument.
        return "lxmusic://songlist/play/%s/%s" % (quote(source, safe=""), quote(identifier, safe="/"))

    def query_playlists(self, playlist_id=None, songs=False):
        connection = self.database()
        try:
            # Bound each field before Python receives it, including Python 3.9
            # where Connection.setlimit is not yet available.
            columns = ["id", "name", "source", "sourceListId"]
            selected = ",".join("substr(%s,1,%d)" % (name, MAX_FIELD_CHARS + 1) for name in columns)
            query = "SELECT " + selected + " FROM my_list"
            if playlist_id is not None:
                query += " WHERE id=?"
                cursor = connection.execute(query + " LIMIT 1", (playlist_id,))
                row = cursor.fetchone()
                if row is None:
                    raise AdapterError("PLAYLIST_NOT_FOUND", "The requested playlist does not exist")
                self.check_fields(row)
                if not songs:
                    return self.playlist_item(row)
                cursor = connection.execute(
                    'SELECT substr(m.name,1,?),substr(m.singer,1,?) FROM my_list_music_info_order o '
                    'JOIN my_list_music_info m ON o.musicInfoId=m.id WHERE o.listId=? '
                    'ORDER BY o."order",o.musicInfoId LIMIT ?',
                    (MAX_FIELD_CHARS + 1, MAX_FIELD_CHARS + 1, playlist_id, MAX_ROWS + 1))
            else:
                cursor = connection.execute(query + " ORDER BY position,id LIMIT ?", (MAX_ROWS + 1,))
            items = []
            total_bytes = 0
            truncated = False
            for row in cursor:
                self.check_fields(row)
                if len(items) == MAX_ROWS:
                    truncated = True
                    break
                item = {"name": row[0], "singer": row[1]} if songs else self.playlist_item(row)
                size = len(json.dumps(item, ensure_ascii=False).encode("utf-8"))
                if total_bytes + size > MAX_OUTPUT_BYTES // 2:
                    truncated = True
                    break
                items.append(item)
                total_bytes += size
            return {"items": items, "truncated": truncated, "limit": MAX_ROWS}
        except (sqlite3.Error, ValueError, TypeError):
            raise AdapterError("DATABASE_QUERY_FAILED", "The music database schema or query is unavailable") from None
        finally:
            connection.close()

    @staticmethod
    def check_fields(row):
        if any(isinstance(value, str) and len(value) > MAX_FIELD_CHARS for value in row):
            raise AdapterError("DATABASE_FIELD_TOO_LARGE", "A music database field exceeds the size limit")

    def playlist_item(self, row):
        return {"id": row[0], "name": row[1], "source": row[2], "sourceId": row[3],
                "playUrl": self.playlist_url(row[2], row[3])}

    def start(self, arguments):
        executable = self.settings.get("executable")
        if not executable or not Path(executable).is_file():
            raise AdapterError("EXECUTABLE_UNAVAILABLE", "Configure an existing LX Music executable")
        if os.name == "nt" and Path(executable).suffix.casefold() in (".bat", ".cmd"):
            raise AdapterError("EXECUTABLE_UNAVAILABLE", "Configure the player executable, not a shell script")
        if os.name != "nt" and not os.access(executable, os.X_OK):
            raise AdapterError("EXECUTABLE_UNAVAILABLE", "The configured LX Music file is not executable")
        options = {"stdin": subprocess.DEVNULL, "stdout": subprocess.DEVNULL,
                   "stderr": subprocess.DEVNULL, "close_fds": True,
                   "cwd": str(Path(executable).parent)}
        if os.name == "nt":
            options["creationflags"] = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
            startupinfo = subprocess.STARTUPINFO()
            startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            startupinfo.wShowWindow = subprocess.SW_HIDE
            options["startupinfo"] = startupinfo
        else:
            options["start_new_session"] = True
        try:
            process = subprocess.Popen([executable] + list(arguments), **options)
        except OSError:
            raise AdapterError("EXECUTABLE_START_FAILED", "The configured LX Music program could not start") from None
        self.dispatched = True
        self.evidence["process_started"] = True
        # A detached player must outlive this one-shot adapter.
        process.poll()

    def verify(self, predicate):
        deadline = time.monotonic() + self.settings["verifyTimeoutMs"] / 1000.0
        last_error = None
        while time.monotonic() < deadline:
            try:
                state = self.status(timeout=deadline - time.monotonic())
                if predicate(state):
                    return state
                last_error = AdapterError("OUTCOME_NOT_VERIFIED", "The observed playback state does not match the request")
            except AdapterError as error:
                last_error = error
            remaining = deadline - time.monotonic()
            if remaining > 0:
                time.sleep(min(0.1, remaining))
        raise last_error or AdapterError("OUTCOME_NOT_VERIFIED", "The write outcome could not be verified")

    @staticmethod
    def playing(status):
        return status["status"].strip().casefold() == "playing"

    @staticmethod
    def text_matches(actual, expected):
        # Exact normalized matching deliberately does not confirm a different
        # song merely because its title contains the requested title.
        return isinstance(actual, str) and unicodedata.normalize("NFKC", actual).strip().casefold() == \
            unicodedata.normalize("NFKC", expected).strip().casefold()

    def execute(self, operation, arguments):
        validate_arguments(operation, arguments)
        try:
            if operation == "status":
                return envelope("completed", {"status": self.status()})
            if operation == "lyric":
                result = self.api("/lyric", response_kind="lyric")
                lyric = result.get("lyric") if isinstance(result, dict) else result
                if not isinstance(lyric, str):
                    raise AdapterError("API_INVALID_LYRIC", "The API response has no lyric text")
                return envelope("completed", {"lyric": lyric})
            if operation in ("list_playlists", "playlist_songs"):
                return envelope("completed", self.query_playlists(arguments.get("playlist_id"), operation == "playlist_songs"))
            if operation == "launch":
                try:
                    self.status()
                    self.evidence["already_ready"] = True
                except AdapterError:
                    self.start([])
                    self.verify(lambda _state: True)
                self.evidence["completion_basis"] = "api_ready"
            elif operation in ("search_play", "play_playlist"):
                if operation == "search_play":
                    song = arguments["song"].strip()
                    artist = arguments.get("artist", "").strip()
                    keyword = song + ("-" + artist if artist else "")
                    url = "lxmusic://music/searchPlay/" + quote(keyword, safe="")
                    predicate = lambda state: self.playing(state) and self.text_matches(state.get("name"), song) and \
                        (not artist or self.text_matches(state.get("singer"), artist))
                    basis = "requested_track_observed"
                else:
                    playlist = self.query_playlists(arguments["playlist_id"])
                    url = playlist["playUrl"]
                    if not url:
                        raise AdapterError("PLAYLIST_NOT_PLAYABLE", "The playlist has no playable source and source ID")
                    predicate = self.playing
                    basis = "playing_observed"
                self.start([url])
                self.evidence["url"] = url
                self.verify(predicate)
                self.evidence["completion_basis"] = basis
            else:
                path = {"play": "/play", "pause": "/pause", "skip_next": "/skip-next",
                        "skip_prev": "/skip-prev", "set_volume": "/volume"}[operation]
                # Mark uncertainty before entering the transport. Even an HTTP
                # error may occur after the player's state has already changed.
                self.dispatched = True
                reply = self.api(path, {"volume": arguments["volume"]} if operation == "set_volume" else None,
                                 response_kind="ack")
                self.evidence["acknowledgement"] = reply
                if operation.startswith("skip_"):
                    self.evidence["completion_basis"] = "api_acknowledged"
                else:
                    if operation == "play":
                        predicate = self.playing
                    elif operation == "pause":
                        predicate = lambda state: state["status"].strip().casefold() == "paused"
                    else:
                        predicate = lambda state: type(state.get("volume")) in (int, float) and \
                            math.isfinite(state["volume"]) and state["volume"] == arguments["volume"]
                    self.verify(predicate)
                    self.evidence["completion_basis"] = "target_state_observed"
            return envelope("completed", self.evidence)
        except AdapterError as error:
            return envelope("unknown" if self.dispatched else "failed", self.evidence, error)

    def diagnose(self):
        data = {"python": platform.python_version(), "platform": platform.system(), "configuration": {"valid": True}}
        executable = self.settings.get("executable")
        exists = bool(executable and Path(executable).is_file())
        executable_ready = (exists and (os.name == "nt" or os.access(executable, os.X_OK))
                            and not (os.name == "nt" and Path(executable).suffix.casefold() in (".bat", ".cmd")))
        data["executable"] = {"configured": bool(executable), "exists": exists, "available": executable_ready}
        try:
            result = self.query_playlists()
            data["database"] = {"available": True, "playlist_count": len(result["items"]), "truncated": result["truncated"]}
        except AdapterError as error:
            data["database"] = {"available": False, "error": error.as_dict()}
        try:
            data["api"] = {"available": True, "status": self.status()}
        except AdapterError as error:
            data["api"] = {"available": False, "error": error.as_dict()}
        available = sum([executable_ready, data["database"]["available"], data["api"]["available"]])
        data["availability"] = "ready" if available == 3 else "partial" if available else "unavailable"
        return envelope("completed", data)


class JsonArgumentParser(argparse.ArgumentParser):
    def error(self, _message):
        raise AdapterError("INVALID_INPUT", "Use --settings with --operation or --diagnose")


def main(argv=None):
    adapter = None
    diagnose = False
    try:
        parser = JsonArgumentParser(add_help=False)
        parser.add_argument("--settings", required=True)
        group = parser.add_mutually_exclusive_group(required=True)
        group.add_argument("--operation")
        group.add_argument("--diagnose", action="store_true")
        options = parser.parse_args(argv)
        diagnose = options.diagnose
        adapter = MusicAdapter(load_settings(options.settings))
        if diagnose:
            result = adapter.diagnose()
        else:
            raw = sys.stdin.buffer.read(MAX_INPUT_BYTES + 1)
            if len(raw) > MAX_INPUT_BYTES:
                raise AdapterError("INVALID_INPUT", "Operation input exceeds the size limit")
            result = adapter.execute(options.operation, decode_json(raw))
    except AdapterError as error:
        data = {"python": platform.python_version(), "platform": platform.system(),
                "configuration": {"valid": False}, "availability": "unavailable"} if diagnose else {}
        result = envelope("unknown" if adapter and adapter.dispatched else "failed", data, error)
    except Exception:
        # Keep protocol output stable and avoid leaking host paths or raw errors.
        result = envelope("unknown" if adapter and adapter.dispatched else "failed", error=
                          AdapterError("ADAPTER_ERROR", "The music adapter could not complete the operation"))
    try:
        encoded = json.dumps(result, ensure_ascii=False, allow_nan=False).encode("utf-8")
        if len(encoded) > MAX_OUTPUT_BYTES:
            raise ValueError("Output too large")
    except (ValueError, UnicodeError, TypeError):
        encoded = json.dumps(envelope("unknown" if adapter and adapter.dispatched else "failed", error=
                             AdapterError("OUTPUT_LIMIT", "The result cannot be represented within the output limit"))).encode("utf-8")
    sys.stdout.buffer.write(encoded + b"\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
