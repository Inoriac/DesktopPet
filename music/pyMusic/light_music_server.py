import ctypes
import os
import queue
import re
import subprocess
import threading
import time
import uuid
from dataclasses import dataclass
from typing import Any, Dict, Optional, Tuple

from flask import Flask, jsonify, request


DEFAULT_PORT = 5010
DEFAULT_PROVIDER = "netease_windows"
NETEASE_WINDOW_KEYWORDS = ["网易云音乐", "NetEase", "CloudMusic"]

VK_MEDIA_NEXT_TRACK = 0xB0
KEYEVENTF_KEYUP = 0x0002
SW_RESTORE = 9


user32 = ctypes.WinDLL("user32", use_last_error=True)


def _send_media_next_track() -> bool:
    user32.keybd_event(VK_MEDIA_NEXT_TRACK, 0, 0, 0)
    user32.keybd_event(VK_MEDIA_NEXT_TRACK, 0, KEYEVENTF_KEYUP, 0)
    return True


def _is_process_running(exe_name: str) -> bool:
    try:
        output = subprocess.check_output(["tasklist"], text=True, encoding="gbk", errors="ignore")
        return exe_name.lower() in output.lower()
    except Exception:
        return False


def _extract_exe_name(path: str) -> str:
    if not path:
        return "cloudmusic.exe"
    return os.path.basename(path).strip() or "cloudmusic.exe"


class _WndCtx(ctypes.Structure):
    _fields_ = [("found", ctypes.c_int), ("hwnd", ctypes.c_void_p)]


WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)


def _find_netease_window() -> Optional[int]:
    result = {"hwnd": None}

    @WNDENUMPROC
    def _enum_proc(hwnd, lparam):
        if not user32.IsWindowVisible(hwnd):
            return True
        length = user32.GetWindowTextLengthW(hwnd)
        if length <= 0:
            return True
        buff = ctypes.create_unicode_buffer(length + 1)
        user32.GetWindowTextW(hwnd, buff, length + 1)
        title = buff.value.strip()
        if not title:
            return True

        for kw in NETEASE_WINDOW_KEYWORDS:
            if kw.lower() in title.lower():
                result["hwnd"] = int(hwnd)
                return False
        return True

    user32.EnumWindows(_enum_proc, 0)
    return result["hwnd"]


def _activate_window(hwnd: int) -> bool:
    if not hwnd:
        return False
    user32.ShowWindow(ctypes.c_void_p(hwnd), SW_RESTORE)
    user32.SetForegroundWindow(ctypes.c_void_p(hwnd))
    return True


def _launch_client_if_needed(client_path: str) -> Tuple[bool, str]:
    exe_name = _extract_exe_name(client_path)

    if _is_process_running(exe_name):
        return True, "already_running"

    if not client_path or not os.path.isfile(client_path):
        return False, "client_not_configured_or_not_found"

    try:
        subprocess.Popen([client_path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception as exc:
        return False, f"launch_failed: {exc}"

    for _ in range(30):
        time.sleep(0.2)
        if _is_process_running(exe_name):
            return True, "launched"

    return False, "launch_timeout"


@dataclass
class ServiceConfig:
    provider: str = DEFAULT_PROVIDER
    netease_client_path: str = ""
    concurrency_policy: str = "replace"  # replace | reject


class CommandExecutor:
    def __init__(self, config: ServiceConfig):
        self.config = config
        self.lock = threading.Lock()
        self.latest_request_id: Optional[str] = None

    def _structured_response(
        self,
        success: bool,
        command: str,
        args: Dict[str, Any],
        request_id: str,
        error: str = "",
        fallback_used: bool = False,
        note: str = "",
        data: Optional[Dict[str, Any]] = None,
    ) -> Dict[str, Any]:
        payload = {
            "success": success,
            "provider": self.config.provider,
            "command": command,
            "args": args,
            "request_id": request_id,
            "error": error,
            "fallback_used": fallback_used,
            "note": note,
        }
        if data is not None:
            payload["data"] = data
        return payload

    def submit(self, command: str, args: Dict[str, Any], request_id: str) -> Dict[str, Any]:
        with self.lock:
            if self.config.concurrency_policy == "reject" and self.latest_request_id is not None:
                return self._structured_response(
                    False,
                    command,
                    args,
                    request_id,
                    error="busy_rejected",
                )
            self.latest_request_id = request_id

        # replace 策略：只执行最新请求，旧请求在执行前检测被覆盖
        return self._execute_latest(command, args, request_id)

    def _is_overridden(self, request_id: str) -> bool:
        with self.lock:
            return self.latest_request_id != request_id

    def _ensure_client_ready(self) -> Tuple[bool, str]:
        return _launch_client_if_needed(self.config.netease_client_path)

    def _execute_latest(self, command: str, args: Dict[str, Any], request_id: str) -> Dict[str, Any]:
        if self._is_overridden(request_id):
            return self._structured_response(
                False, command, args, request_id, error="request_replaced_before_execute"
            )

        ready, ready_note = self._ensure_client_ready()
        if not ready:
            return self._structured_response(
                False, command, args, request_id, error=ready_note
            )

        if self._is_overridden(request_id):
            return self._structured_response(
                False, command, args, request_id, error="request_replaced"
            )

        if command == "next_track":
            _send_media_next_track()
            return self._structured_response(
                True,
                command,
                args,
                request_id,
                note=ready_note,
                data={"control": "media_next_track"},
            )

        if command == "play_song":
            return self._handle_play_song(args, request_id, ready_note)

        if command == "switch_playlist":
            return self._handle_switch_playlist(args, request_id, ready_note)

        return self._structured_response(False, command, args, request_id, error="unknown_command")

    def _handle_play_song(self, args: Dict[str, Any], request_id: str, ready_note: str) -> Dict[str, Any]:
        song_id = (args.get("song_id") or "").strip()
        song_url = (args.get("song_url") or "").strip()
        song = (args.get("song") or "").strip()
        artist = (args.get("artist") or "").strip()

        if not song_id and not song_url and not song:
            return self._structured_response(False, "play_song", args, request_id, error="missing_song_identifier")

        # 一级：尝试 deep-link / url 启动
        if song_id:
            deep_link = f"orpheus://song/{song_id}"
            ok, msg = self._open_uri(deep_link)
            if ok:
                return self._structured_response(
                    True,
                    "play_song",
                    args,
                    request_id,
                    note=f"deep_link_song_id:{song_id}",
                    data={"mode": "deep_link", "uri": deep_link},
                )

        if song_url:
            ok, msg = self._open_uri(song_url)
            if ok:
                return self._structured_response(
                    True,
                    "play_song",
                    args,
                    request_id,
                    note="song_url_opened",
                    data={"mode": "song_url", "uri": song_url},
                )

        # 二级降级：当前先返回可解析命令，交给上层/后续 UI 自动化执行
        query = f"{song} {artist}".strip()
        return self._structured_response(
            True,
            "play_song",
            args,
            request_id,
            fallback_used=True,
            note="fallback_to_local_search_needed",
            data={
                "mode": "fallback_search",
                "query": query,
                "match_policy": "exact_song_artist_then_first",
            },
        )

    def _handle_switch_playlist(self, args: Dict[str, Any], request_id: str, ready_note: str) -> Dict[str, Any]:
        playlist_id = (args.get("playlist_id") or "").strip()
        if not playlist_id:
            return self._structured_response(False, "switch_playlist", args, request_id, error="missing_playlist_id")

        # 尝试网易云歌单链接
        playlist_url = f"https://music.163.com/#/playlist?id={playlist_id}"
        ok, msg = self._open_uri(playlist_url)
        if not ok:
            return self._structured_response(
                False,
                "switch_playlist",
                args,
                request_id,
                error=f"open_playlist_failed:{msg}",
            )

        return self._structured_response(
            True,
            "switch_playlist",
            args,
            request_id,
            note=ready_note,
            data={"mode": "playlist_url", "url": playlist_url},
        )

    @staticmethod
    def _open_uri(uri: str) -> Tuple[bool, str]:
        try:
            os.startfile(uri)  # type: ignore[attr-defined]
            return True, "ok"
        except Exception as exc:
            return False, str(exc)


app = Flask(__name__)
service_config = ServiceConfig(
    provider=os.environ.get("MUSIC_PROVIDER", DEFAULT_PROVIDER),
    netease_client_path=os.environ.get("NETEASE_CLIENT_PATH", ""),
    concurrency_policy=os.environ.get("MUSIC_CONCURRENCY", "replace").strip().lower() or "replace",
)
if service_config.concurrency_policy not in ("replace", "reject"):
    service_config.concurrency_policy = "replace"

executor = CommandExecutor(service_config)


@app.get("/health")
def health() -> Any:
    return jsonify(
        {
            "ok": True,
            "provider": service_config.provider,
            "concurrency_policy": service_config.concurrency_policy,
            "has_client_path": bool(service_config.netease_client_path),
        }
    )


@app.post("/command")
def command() -> Any:
    payload = request.get_json(silent=True) or {}
    command_name = (payload.get("command") or "").strip()
    args = payload.get("args") or {}
    request_id = (payload.get("request_id") or str(uuid.uuid4())).strip()

    if not command_name:
        return jsonify(
            {
                "success": False,
                "provider": service_config.provider,
                "command": "",
                "args": args,
                "request_id": request_id,
                "error": "missing_command",
            }
        ), 400

    result = executor.submit(command_name, args, request_id)
    status = 200 if result.get("success") else 409 if result.get("error") == "busy_rejected" else 200
    return jsonify(result), status


if __name__ == "__main__":
    port = int(os.environ.get("MUSIC_SERVICE_PORT", str(DEFAULT_PORT)))
    app.run(host="127.0.0.1", port=port, debug=False)

