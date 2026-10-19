"""Read-only Bilibili API access; dependencies are imported only when needed.

The search/subtitle flow follows dsh-bili-scroll's bilibili-mcp integration.
See THIRD_PARTY_NOTICES.md and licenses/. No platform write API is exposed.
"""
import asyncio
import html
import json
from pathlib import Path
import re
from types import SimpleNamespace
from urllib.parse import urlsplit


class BilibiliError(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code
        self.message = message


def read_credential(settings):
    path = Path(settings["credentialPath"])
    if not path.exists():
        return {}
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(value, dict) or not all(isinstance(v, str) for v in value.values()):
            raise ValueError("Invalid credential")
        return value
    except (OSError, ValueError):
        raise BilibiliError("INVALID_CREDENTIAL", "Credential file cannot be read; run the login helper again") from None


def load_library(settings):
    try:
        from bilibili_api import Credential, comment, request_settings, search, video
    except ImportError:
        raise BilibiliError("DEPENDENCY_MISSING", "Prepare the plugin Python environment before reading Bilibili") from None
    request_settings.set_timeout(settings["requestTimeoutSeconds"])
    # One attempt; a later tool call is a deliberate decision by the subject.
    request_settings.set_wbi_retry_times(1)
    cookies = read_credential(settings)
    credential = Credential(sessdata=cookies.get("SESSDATA", cookies.get("sessdata")),
                            bili_jct=cookies.get("bili_jct"), buvid3=cookies.get("buvid3"),
                            buvid4=cookies.get("buvid4"),
                            dedeuserid=cookies.get("DedeUserID", cookies.get("dedeuserid")),
                            ac_time_value=cookies.get("ac_time_value"))
    return SimpleNamespace(video=video, search=search, comment=comment, credential=credential)


def translate_error(error):
    """Classify errors without reflecting remote bodies, cookies or signed URLs."""
    if isinstance(error, BilibiliError):
        return error
    if isinstance(error, (asyncio.TimeoutError, TimeoutError)):
        return BilibiliError("NETWORK_TIMEOUT", "The Bilibili request timed out")
    code = getattr(error, "code", None)
    status = getattr(error, "status", None)
    if code in (-101, -102, -111) or type(error).__name__.startswith("Credential"):
        return BilibiliError("AUTH_REQUIRED", "Bilibili login is required or expired; run the login helper")
    if code in (-412, -509) or status in (412, 429):
        return BilibiliError("RATE_LIMITED", "Bilibili limited this request; try again later")
    if code is not None:
        return BilibiliError("PLATFORM_ERROR", "Bilibili rejected the request (code %s)" % code)
    if isinstance(error, (OSError, ConnectionError)) or status is not None or type(error).__module__.startswith("aiohttp"):
        return BilibiliError("NETWORK_ERROR", "The Bilibili network request failed")
    return BilibiliError("BACKEND_ERROR", "Bilibili returned an unexpected result")


def clean_title(value):
    return html.unescape(re.sub(r"<[^>]*>", "", value))


class BilibiliBackend:
    def __init__(self, settings):
        self.settings = settings

    async def search(self, keyword, page, limit, order):
        api = load_library(self.settings)
        order_type = {"totalrank": api.search.OrderVideo.TOTALRANK,
                      "click": api.search.OrderVideo.CLICK,
                      "pubdate": api.search.OrderVideo.PUBDATE,
                      "dm": api.search.OrderVideo.DM}[order]
        result = await api.search.search_by_type(keyword=keyword,
                                                search_type=api.search.SearchObjectType.VIDEO,
                                                order_type=order_type, page=page, page_size=limit)
        entries = [{"bvid": item["bvid"], "title": clean_title(item["title"]),
                    "author": item.get("author", ""), "duration": item.get("duration", ""),
                    "description": item.get("description", "")}
                   for item in result["result"][:limit]]
        total = int(result["numResults"])
        return {"entries": entries, "total": total, "hasMore": page < int(result["numPages"])}

    async def video_info(self, bvid, part):
        api = load_library(self.settings)
        value = await api.video.Video(bvid=bvid, credential=api.credential).get_info()
        pages = value["pages"]
        if part > len(pages):
            raise BilibiliError("INVALID_INPUT", "The requested video part does not exist")
        selected = pages[part - 1]
        return {"bvid": bvid, "aid": value["aid"], "cid": selected["cid"], "part": part,
                "partCount": len(pages), "title": value["title"], "partTitle": selected["part"],
                "durationSeconds": selected["duration"], "description": value.get("desc", ""),
                "author": value.get("owner", {}).get("name", "")}

    async def comments(self, bvid, page, limit):
        info = await self.video_info(bvid, 1)
        api = load_library(self.settings)
        value = await api.comment.get_comments(oid=info["aid"], type_=api.comment.CommentResourceType.VIDEO,
                                               page_index=page, order=api.comment.OrderType.LIKE,
                                               credential=api.credential)
        replies = value["replies"] or []
        page_info = value["page"]
        return {"entries": [{"id": str(item["rpid"]), "userId": str(item["member"]["mid"]),
                              "author": item["member"]["uname"], "text": item["content"]["message"],
                              "likes": item.get("like", 0), "time": item.get("ctime", 0)}
                             for item in replies[:limit]],
                "total": page_info["count"],
                "hasMore": page_info["num"] * page_info["size"] < page_info["count"],
                "truncated": len(replies) > limit}

    async def transcript(self, bvid, part):
        info = await self.video_info(bvid, part)
        api = load_library(self.settings)
        result = await api.video.Video(bvid=bvid, credential=api.credential).get_subtitle(cid=info["cid"])
        tracks = result["subtitles"]
        if not tracks:
            return {"video": info, "availability": "unavailable", "reason": "no_subtitle", "segments": []}
        chosen = next((track for track in tracks if track.get("lan") in ("ai-zh", "zh-CN", "zh-Hans", "zh")), tracks[0])
        url = chosen["subtitle_url"]
        if url.startswith("//"):
            url = "https:" + url
        body = await self.download_subtitle(url)
        segments = [{"startMs": round(item["from"] * 1000), "endMs": round(item["to"] * 1000),
                     "text": item["content"]} for item in body["body"]]
        return {"video": info, "availability": "available" if segments else "unavailable",
                "reason": "" if segments else "no_subtitle", "language": chosen.get("lan", ""),
                "segments": segments}

    async def download_subtitle(self, url):
        # The URL comes only from Bilibili's subtitle response, never model input.
        parsed = urlsplit(url)
        if parsed.scheme not in ("https", "http") or not parsed.hostname or parsed.username or parsed.password:
            raise BilibiliError("PLATFORM_ERROR", "Bilibili returned an invalid subtitle location")
        try:
            import aiohttp
        except ImportError:
            raise BilibiliError("DEPENDENCY_MISSING", "The subtitle reader requires aiohttp") from None
        timeout = aiohttp.ClientTimeout(total=self.settings["requestTimeoutSeconds"])
        async with aiohttp.ClientSession(timeout=timeout) as session:
            async with session.get(url) as response:
                response.raise_for_status()
                # Long videos are cached as text, not injected in one model request.
                chunks, size = [], 0
                limit = 16 * 1024 * 1024
                while True:
                    chunk = await response.content.read(min(65536, limit + 1 - size))
                    if not chunk:
                        break
                    chunks.append(chunk)
                    size += len(chunk)
                    if size > limit:
                        raise BilibiliError("CONTENT_TOO_LARGE", "Subtitle content exceeds the local snapshot limit")
                return json.loads(b"".join(chunks))
