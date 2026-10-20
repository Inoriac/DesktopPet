"""Bounded OneBot v11 forward WebSocket client with verified account identity."""
import sys
sys.dont_write_bytecode = True

from collections import deque
import time
import uuid

if __package__:
    from .common import AdapterError, decode_json, json_text, qq_id
else:
    from common import AdapterError, decode_json, json_text, qq_id


class OneBotClient:
    def __init__(self, settings, capture_events=False):
        self.settings = settings
        self.capture_events = capture_events
        self.socket = None
        self.deadline = None
        self.events = deque()

    def __enter__(self):
        try:
            from websockets.sync.client import connect
        except ImportError:
            raise AdapterError("DEPENDENCY_UNAVAILABLE", "Run QQ plugin setup with its configured Python interpreter") from None
        headers = {}
        if self.settings.get("tokenFile"):
            try:
                with open(self.settings["tokenFile"], "r", encoding="utf-8") as source:
                    token = source.read(8193).strip()
                if not token or len(token) > 8192 or any(ord(c) < 32 or ord(c) > 126 for c in token):
                    raise ValueError()
                headers["Authorization"] = "Bearer " + token
            except (OSError, ValueError, UnicodeError):
                raise AdapterError("INVALID_SETTINGS", "tokenFile must contain a readable, nonempty access token") from None
        self.deadline = time.monotonic() + self.settings["requestTimeoutSeconds"]
        try:
            self.socket = connect(self.settings["wsUrl"], additional_headers=headers,
                                  open_timeout=self.remaining(), close_timeout=1,
                                  max_size=1024 * 1024, max_queue=16, proxy=None)
            login = self.call("get_login_info", {})
            if not isinstance(login, dict) or qq_id(login.get("user_id")) != self.settings["selfId"]:
                raise AdapterError("ACCOUNT_MISMATCH", "OneBot is logged into a different QQ account")
            return self
        except AdapterError:
            self.close()
            raise
        except Exception:
            self.close()
            raise AdapterError("NETWORK_ERROR", "Cannot establish the configured OneBot connection") from None

    def __exit__(self, *_):
        self.close()

    def close(self):
        if self.socket:
            self.socket.close()
            self.socket = None
        self.events.clear()

    def remaining(self):
        left = self.deadline - time.monotonic()
        if left <= 0:
            raise AdapterError("TIMEOUT", "OneBot operation timed out")
        return left

    def call(self, action, params):
        echo = uuid.uuid4().hex
        try:
            self.remaining()
            self.socket.send(json_text({"action": action, "params": params, "echo": echo}))
            while True:
                value = decode_json(self.socket.recv(timeout=self.remaining()))
                if not isinstance(value, dict) or value.get("echo") != echo:
                    # Hold pushes until login has verified the connection.
                    # Short command callers never consume or ingest these.
                    if self.capture_events and isinstance(value, dict) and "post_type" in value:
                        if len(self.events) >= 64:
                            raise AdapterError("PROTOCOL_ERROR", "Too many OneBot events before the action response")
                        self.events.append(value)
                    continue
                status, retcode = value.get("status"), value.get("retcode")
                if status == "ok" and type(retcode) is int and retcode == 0:
                    return value.get("data")
                if status == "failed" and type(retcode) is int and retcode != 0:
                    code = "UNSUPPORTED_ACTION" if retcode in (1404, 404) else "API_REJECTED"
                    raise AdapterError(code, "OneBot rejected the requested action")
                # An async acknowledgement or malformed response isn't an
                # explicit rejection. A caller that sent a message must keep
                # its outcome unknown rather than encourage a retry.
                raise AdapterError("PROTOCOL_ERROR", "OneBot did not return an unambiguous action outcome")
        except AdapterError:
            raise
        except TimeoutError:
            raise AdapterError("TIMEOUT", "OneBot operation timed out") from None
        except (ValueError, UnicodeError):
            raise AdapterError("PROTOCOL_ERROR", "OneBot returned invalid JSON") from None
        except Exception:
            raise AdapterError("NETWORK_ERROR", "OneBot connection was interrupted") from None

    def receive(self, timeout=.1):
        if self.events:
            return self.events.popleft()
        try:
            value = decode_json(self.socket.recv(timeout=timeout))
            return value if isinstance(value, dict) and "post_type" in value else None
        except TimeoutError:
            return None
        except (ValueError, UnicodeError):
            raise AdapterError("PROTOCOL_ERROR", "OneBot returned invalid JSON") from None
        except Exception:
            raise AdapterError("NETWORK_ERROR", "OneBot connection was interrupted") from None
