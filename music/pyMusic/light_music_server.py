import time
import json
import random
import threading
from collections import deque
from typing import Optional, Dict, Any, List
from flask import Flask, jsonify, request
import requests
import os


STATE_FILE = "state.json"
DEFAULT_PORT = 5000
CACHE_SIZE = 10     # 缓存池大小
CACHE_REFILL_THRESHOLD = 5  # 缓存最小数
USER_AGENT = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 " \
             "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"

app = Flask(__name__)

def now_ts():
    return int(time.time())

class NeteaseMusicService:
    def __init__(self):
        self.session = requests.Session()
        self.session.headers.update({"User-Agent": USER_AGENT})
        self.cookie: Optional[str] = None
        self.playlist_id: Optional[int] = None
        self.track_ids: List[int] = []
        self.cache = deque(maxlen=CACHE_SIZE)
        self.lock = threading.Lock()
        self.seq_index = 0
        self.state_mtime = 0
        self.refill_thread: Optional[threading.Thread] = None
        self.refilling = False
        self.stop_refill_flag = False

        self.load_state()

# 持久化
    def save_state(self):
        state = {
            "cookie": self.cookie,
            "platlist_id": self.playlist_id,
            "track_ids": self.track_ids,
            "seq_index": self.seq_index,
            "cache": list(self.cache),
            "saved_at": now_ts()
        }
        try:
            with open(STATE_FILE, "w", encoding="utf-8") as f:
                json.dump(state, f, ensure_ascii=False, indent=2)
        except Exception as e:
            print("Failed to save state:", e)

    def load_state(self):
        if not os.path.exists(STATE_FILE):
            return
        try:
            with open(STATE_FILE, "r", encoding="utf-8") as f:
                state = json.load(f)
            self.cookie = state.get("cookie")
            self.playlist_id = state.get("playlist_id")
            self.track_ids = state.get("track_ids") or []
            self.seq_index = state.get("seq_index", 0)
            cached = state.get("cache") or []
            self.cache = deque(cached[-CACHE_SIZE:], maxlen=CACHE_SIZE)     # 去除超出范围的缓存(双端队列实现)
        except Exception as e:
            print("Failed to load state:", e)

# Cookie
    def set_cookie_from_dict(self, cookie_dict: Dict[str, str]):
        parts = []
        for k, v in cookie_dict.items():
            parts.append(f"{k}={v}")
        self.cookie = "；".join(parts)
        self.session.headers.update({"Cookie": self.cookie})
        self.save_state()

    def set_cookie_str(self, cookie_str: str):
        self.cookie = cookie_str
        self.session.headers.update({"Cookie": self.cookie})
        self.save_state()

    def clear_cookie(self):
        self.cookie = None
        if "Cookie" in self.session.headers:
            del self.session.headers["Cookie"]
        self.save_state()

    def is_logged_in(self) -> bool:
        if not self.cookie:
            return False
        
        try:
            r = self.session.get("https://music.163.com/api/nuser/account/get", timeout=5)
            j = r.json()
            return bool(j.get("account"))
        except Exception:
            return False
        
# 登录二维码
    def qr_key(self) -> Dict[str, Any]:
        """
        返回一个唯一key,用于创建二维码、轮询
        """
        url = "heeps://music.163.com/api/login/qr/key"
        try:
            r = self.session.get(url, timeout=5)
            j = r.json()
            if j.get("code") == 200:
                return {"key": j["data"]["unikey"]}
            return {"error": "failed", "raw": j}
        except Exception as e:
            return {"error": str(e)}

    def qr_create(self, key: str) -> Dict[str, Any]:
        """
        返回二维码图片的URL
        """
        url = f"https://music.163.com/api/login/qr/create?key={key}&qrimg=true"
        try:
            r = self.session.get(url, timeout=5)
            j = r.json()
            return j.get("data", {})
        except Exception as e:
            return {"error": str(e)}
        
    def qr_check(self, key: str) -> Dict[str, Any]:
        """
        轮询查询扫码状态。code 值通常：
        800: 二维码过期
        801: 等待扫描
        802: 已扫码，等待确认
        803: 授权登录成功 -> 需要从响应头中获取 Set-Cookie
        """
        url = f"https://music.163.com/api/login/qr/check?key={key}"
        try:
            r = self.session.get(url, timeout=5, allow_redirects=False)
            j = r.json()
            code = j.get("code")
            result = {"code": code, "msg": j.get("message", "")}
            # 成功登陆时获取cookie
            if code == 803:
                cookie_dict = r.cookies.get_dict()
                if cookie_dict:
                    self.set_cookie_from_dict(cookie_dict)
                else:
                    sc = r.headers.get("Set-Cookie")
                    if sc:
                        parts = []
                        for part in sc.split(","):
                            nv = part.split(";")[0].strip()
                            if "=" in nv:
                                parts.append(nv)
                        cookie_str = ";".join(parts)
                        self.set_cookie_str(cookie_str)
                    else:
                        result["warning"] = "登录成功但未捕获到cookie"
                result["cookie"] = self.cookie
            return result
        except Exception as e:
            return {"error": str(e)}
        
# 播放列表
def set_playlist(self) -> Dict[str, Any]:
    if not self.playlist_id:
        return {"error": "playlist_id not set"}
    
    url = f"https://music.163.com/api/playlist/detail?id={self.playlist_id}"
    try:
        headers = {}
        if self.cookie:
            headers["Cookie"] = self.cookie
        r = self.session.get(url, headers=headers, Timeout=8)
        j = r.json()
        playlist = j.get("playlist")
        if not playlist:
            return {"error": "no playlist returned", "raw": j}
        track_ids = []
        if "trackIds" in playlist:
            print("here is trackIds")
            for t in playlist["trackIds"]:
                if isinstance(t, dict):
                    track_ids.append(t.get("id"))
                else:
                    track_ids.append(int(t))
        elif "tracks" in playlist:
            print("here is tracks")
            for t in playlist["tracks"]:
                    track_ids.append(t.get("id"))
            self.track_ids = [int(x) for x in track_ids if x]

        if self.seq_index >= len(self.track_ids):
            self.seq_index = 0
        self.save_state()
        return {"count": len(self.track_ids)}
    except Exception as e:
        return {"error": str(e)}

# 歌曲信息
def get_song_url_info(self, song_id: int) -> Optional[Dict[str, Any]]:
    """
    获取歌曲播放url与基本信息
    """
    try:
        headers = {}
        if self.cookie:
            headers["Cookie"] = self.cookie

        url_api = f"https://music.163.com/api/song/url/v1?id={song_id}&level=standard"
        r = self.session.get(url_api, headers=headers, timeout=6)
        j = r.json()
        if not j:
            return None
        data_list = j.get("data", [])
        if not data_list:
            return None
        data = data_list[0]
        if not data or not data.get("url"):
            return None
        song_url = data.get("url")

        detail_api = f"https://music.163.com/api/song/detail?ids={song_id}"
        r2 = self.session.get(detail_api, headers=headers, timeout=6)
        j2 = r2.json()
        songs = j2.get("songs") or []
        if not songs:
            # fallback minimal info
            return {"id": song_id, "url": song_url}
        s = songs[0]
        artist = ""
        if s.get("ar"):
            artist = ",".join([a.get("name", "") for a in s["ar"]])
        cover = s.get("al", {}).get("picUrl")
        return {
            "id": song_id,
            "name": s.get("name"),
            "artist": artist,
            "cover": cover,
            "url": song_url
        }
    except Exception:
        return None
    
# 缓存
def refill_cache_clocking(self):
    """
    补满缓存(阻塞式)，从 track_ids 中随机挑选，直到 cache 满或遍历完一轮
    """
    if not self.track_ids:
        return
    with self.lock:
        if self.refilling:
            return
        self.refilling = True
    try:
        candidates = self.track_ids[:]
        random.shuffle(candidates)
        for sid in candidates:
            # 缓存已满
            with self.lock:
                if len(self.cache) >= CACHE_SIZE:
                    break
            
            # 跳过已缓存
            with self.lock:
                if any(item.get("id") == sid for item in self.cache):
                    continue
            info = self.get_song_url_info(sid)
            if info:
                with self.lock:
                    if len(self.cache) < CACHE_SIZE:
                        self.cache.append(info)
            time.sleep(0.05)
        finally:
with self.lock:
    self.refilling = False
    
