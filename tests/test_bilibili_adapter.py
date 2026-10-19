"""Bilibili read behavior, without an account or third-party dependencies."""
import asyncio
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from plugins.bilibili import adapter, backend

BV = "BV1xx411c7mD"
READ_OPERATIONS = {"status", "search", "video_info", "comments", "read_transcript"}


class FakeBackend:
    def __init__(self):
        self.calls = []
        self.error = None
        self.segments = [{"startMs": 1250, "endMs": 4500, "text": "开始🙂"}]
        self.info = {"bvid": BV, "aid": 12, "cid": 202, "part": 2,
                     "partCount": 2, "title": "测试视频", "partTitle": "第二集",
                     "durationSeconds": 60, "description": "简介", "author": "作者"}

    def check(self, method, arguments):
        self.calls.append((method, arguments))
        if self.error:
            raise self.error

    async def search(self, keyword, page, limit, order):
        self.check("search", (keyword, page, limit, order))
        return {"entries": [{"bvid": BV, "title": keyword, "author": "作者",
                              "description": "简介", "duration": "01:00"}],
                "total": 1, "hasMore": False}

    async def video_info(self, bvid, part):
        self.check("video_info", (bvid, part))
        if part > 2:
            raise backend.BilibiliError("INVALID_INPUT", "Unknown video part")
        return dict(self.info, bvid=bvid, part=part, cid=101 if part == 1 else 202)

    async def comments(self, bvid, page, limit):
        self.check("comments", (bvid, page, limit))
        return {"entries": [{"id": "31", "userId": "99", "author": "用户",
                              "text": "评论🙂", "likes": 2, "time": 123}],
                "total": 1, "hasMore": False}

    async def transcript(self, bvid, part):
        self.check("transcript", (bvid, part))
        return {"video": dict(self.info, bvid=bvid, part=part),
                "availability": "available" if self.segments else "unavailable",
                "reason": "no_subtitle" if not self.segments else "",
                "language": "zh-CN", "segments": self.segments}


def utf16(value):
    return len(value.encode("utf-16-le")) // 2


class AdapterTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="bilibili 中文 ")
        self.addCleanup(self.temp.cleanup)
        self.settings = {"stateDir": self.temp.name}
        self.backend = FakeBackend()

    def run_op(self, operation, arguments=None):
        return adapter.run(operation, arguments or {}, self.settings, self.backend)

    def assert_budget(self, value):
        if isinstance(value, str):
            self.assertLessEqual(utf16(value), 1800)
        elif isinstance(value, dict):
            for child in value.values():
                self.assert_budget(child)
        elif isinstance(value, list):
            for child in value:
                self.assert_budget(child)

    def test_settings_defaults_and_invalid_paths(self):
        settings = adapter.validate_settings(self.settings)
        self.assertEqual(settings["requestTimeoutSeconds"], 15)
        self.assertFalse(settings["asrEnabled"])
        self.assertEqual(settings["credentialPath"], str(Path(self.temp.name).resolve() / "credential.json"))
        for delta in ({"stateDir": "relative"}, {"credentialPath": "relative"},
                      {"stateDir": str(ROOT / "plugins" / "bilibili" / "cache")},
                      {"asrEnabled": "yes"}, {"testMode": True},
                      {"requestTimeoutSeconds": True}, {"asrPython": "python"}):
            with self.subTest(delta=delta), self.assertRaises(adapter.AdapterError):
                adapter.validate_settings(dict(self.settings, **delta))

    def test_status_missing_dependencies_does_not_call_network_or_write(self):
        with patch.object(adapter.importlib.util, "find_spec", return_value=None):
            result = self.run_op("status")
        self.assertEqual(result["state"], "completed")
        self.assertEqual(result["data"]["availability"], "unavailable")
        self.assertEqual(result["data"]["missingDependencies"], ["bilibili-api-python", "aiohttp"])
        self.assertFalse(result["data"]["credentialPresent"])
        self.assertEqual(self.backend.calls, [])
        self.assertEqual(list(Path(self.temp.name).iterdir()), [])

    def test_search_normal_defaults_and_unicode(self):
        result = self.run_op("search", {"keyword": "自然🙂"})
        self.assertEqual(result["state"], "completed")
        self.assertEqual(self.backend.calls, [("search", ("自然🙂", 1, 5, "totalrank"))])
        entry = result["data"]["entries"][0]
        self.assertEqual(entry["bvid"], BV)
        self.assertEqual(entry["url"], "https://www.bilibili.com/video/" + BV)
        self.assertEqual(result["data"]["source"], "bilibili_search")

    def test_second_part_info_and_unknown_part(self):
        result = self.run_op("video_info", {"bvid": BV, "part": 2})
        self.assertEqual(result["data"]["cid"], 202)
        self.assertEqual(result["data"]["part"], 2)
        self.assertEqual(result["data"]["partCount"], 2)
        result = self.run_op("video_info", {"bvid": BV, "part": 3})
        self.assertEqual(result["data"]["error"]["code"], "INVALID_INPUT")

    def test_comments_empty_is_not_failure(self):
        result = self.run_op("comments", {"bvid": BV})
        self.assertEqual(result["data"]["entries"][0]["text"], "评论🙂")
        self.backend.error = backend.BilibiliError("RATE_LIMITED", "Platform rate limit")
        result = self.run_op("comments", {"bvid": BV})
        self.assertEqual(result["state"], "failed")
        self.assertNotIn("entries", result["data"])
        async def empty(*args):
            return {"entries": [], "total": 0, "hasMore": False}
        self.backend.comments = empty
        self.assertEqual(self.run_op("comments", {"bvid": BV})["data"]["entries"], [])

    def test_errors_are_stable_and_do_not_expose_remote_messages(self):
        for error, code in ((backend.BilibiliError("AUTH_REQUIRED", "Login required"), "AUTH_REQUIRED"),
                            (asyncio.TimeoutError("SESSDATA=secret"), "NETWORK_TIMEOUT"),
                            (RuntimeError("signed-url secret"), "BACKEND_ERROR")):
            with self.subTest(code=code):
                self.backend.error = error
                result = self.run_op("search", {"keyword": "test"})
                self.assertEqual(result["data"]["error"]["code"], code)
                self.assertNotIn("secret", json.dumps(result))

    def test_bad_arguments_and_platform_writes_never_dispatch(self):
        cases = [("search", {"keyword": ""}), ("search", {"keyword": "x", "limit": 6}),
                 ("search", {"keyword": "x", "page": True}),
                 ("search", {"keyword": "x", "order": "invalid"}),
                 ("video_info", {"bvid": "https://bad.invalid"}),
                 ("video_info", {"bvid": BV, "part": 0}),
                 ("read_transcript", {"bvid": BV, "source": "random"}),
                 ("read_transcript", {"bvid": BV, "cursor": "../secret"}),
                 ("comments", {"bvid": BV, "url": "http://bad.invalid"}),
                 ("send_comment", {"bvid": BV}), ("login", {})]
        for operation, arguments in cases:
            with self.subTest(operation=operation, arguments=arguments):
                self.assertEqual(self.run_op(operation, arguments)["state"], "failed")
        self.assertEqual(self.backend.calls, [])

    def test_transcript_pages_preserve_all_text_timestamps_and_source(self):
        text = ('长段🙂"\\\n' * 900) + "尾"
        self.backend.segments = [{"startMs": 1250, "endMs": 4500, "text": text},
                                 {"startMs": 5000, "endMs": 6000, "text": "下一段"}]
        all_segments = []
        arguments = {"bvid": BV, "part": 2}
        pages = 0
        while True:
            result = self.run_op("read_transcript", arguments)
            self.assertEqual(result["state"], "completed", result)
            data = result["data"]
            self.assertEqual((data["bvid"], data["part"], data["cid"], data["source"]),
                             (BV, 2, 202, "platform_subtitle"))
            self.assert_budget(data)
            self.assertLessEqual(utf16(json.dumps(data, ensure_ascii=False, separators=(",", ":"))), 3800)
            self.assertLess(utf16(json.dumps({"stage": "completed", "data": data}, ensure_ascii=False)), 6000)
            all_segments.extend(data["segments"])
            pages += 1
            if not data["hasMore"]:
                break
            arguments["cursor"] = data["nextCursor"]
            self.assertLess(pages, 100)
        self.assertGreater(pages, 1)
        self.assertEqual("".join(s["text"] for s in all_segments), text + "下一段")
        self.assertTrue(all(s["startMs"] == 1250 and s["endMs"] == 4500 for s in all_segments[:-1]))
        self.assertTrue(all(len(s["text"]) <= 800 for s in all_segments))
        self.assertEqual(len(self.backend.calls), 1)

    def test_no_subtitle_is_distinct_from_network_failure(self):
        self.backend.segments = []
        result = self.run_op("read_transcript", {"bvid": BV})
        self.assertEqual(result["state"], "completed")
        self.assertEqual(result["data"]["availability"], "unavailable")
        self.assertEqual(result["data"]["reason"], "no_subtitle")
        self.backend.error = backend.BilibiliError("NETWORK_ERROR", "Network failure")
        self.assertEqual(self.run_op("read_transcript", {"bvid": BV})["state"], "failed")

    def first_cursor(self):
        self.backend.segments = [{"startMs": 0, "endMs": 1000, "text": "字幕🙂" * 3000}]
        return self.run_op("read_transcript", {"bvid": BV, "part": 2})["data"]["nextCursor"]

    def test_cursor_rejects_wrong_video_part_source_and_account(self):
        cursor = self.first_cursor()
        arguments = {"bvid": BV, "part": 2, "cursor": cursor}
        for delta in ({"bvid": "BV1yy411c7mD"}, {"part": 1}, {"source": "local_asr"},
                      {"cursor": cursor[:-2] + "xx"}):
            with self.subTest(delta=delta):
                self.assertEqual(self.run_op("read_transcript", dict(arguments, **delta))["state"], "failed")
        Path(self.temp.name, "credential.json").write_text('{"SESSDATA":"changed-secret"}', encoding="utf-8")
        result = self.run_op("read_transcript", arguments)
        self.assertEqual(result["data"]["error"]["code"], "INVALID_CURSOR")
        self.assertEqual(len(self.backend.calls), 1)

    def test_deleted_or_corrupt_cache_asks_for_first_page(self):
        cursor = self.first_cursor()
        for path in Path(self.temp.name, "transcripts").glob("*.json"):
            path.write_text("broken", encoding="utf-8")
        result = self.run_op("read_transcript", {"bvid": BV, "part": 2, "cursor": cursor})
        self.assertEqual(result["data"]["error"]["code"], "INVALID_CURSOR")
        self.assertEqual(len(self.backend.calls), 1)
        repaired = self.run_op("read_transcript", {"bvid": BV, "part": 2})
        self.assertEqual(repaired["state"], "completed", repaired)

    def test_account_change_during_read_cannot_publish_to_new_account(self):
        original = self.backend.transcript
        async def changed(bvid, part):
            value = await original(bvid, part)
            Path(self.temp.name, "credential.json").write_text('{"SESSDATA":"new-account"}', encoding="utf-8")
            return value
        self.backend.transcript = changed
        result = self.run_op("read_transcript", {"bvid": BV, "part": 2})
        self.assertEqual(result["data"]["error"]["code"], "CREDENTIAL_CHANGED")
        self.assertFalse(Path(self.temp.name, "transcripts").exists())

    def test_cache_write_error_is_a_state_error_not_empty_subtitles(self):
        with patch.object(adapter.os, "replace", side_effect=OSError("secret path")):
            result = self.run_op("read_transcript", {"bvid": BV, "part": 2})
        self.assertEqual(result["data"]["error"]["code"], "STATE_UNAVAILABLE")
        self.assertNotIn("secret", json.dumps(result))

    def test_full_operation_timeout_is_bounded(self):
        self.settings["requestTimeoutSeconds"] = 1
        async def slow(*args):
            await asyncio.sleep(10)
        self.backend.search = slow
        result = self.run_op("search", {"keyword": "test"})
        self.assertEqual(result["data"]["error"]["code"], "NETWORK_TIMEOUT")

    def test_many_escaped_control_characters_are_read_without_loss(self):
        self.backend.segments = [{"startMs": 0, "endMs": 1, "text": "\x01" * 900}]
        result = self.run_op("read_transcript", {"bvid": BV})
        self.assertEqual(result["state"], "completed", result)
        text = "".join(item["text"] for item in result["data"]["segments"])
        cursor = result["data"]["nextCursor"]
        while cursor:
            result = self.run_op("read_transcript", {"bvid": BV, "cursor": cursor})
            self.assertEqual(result["state"], "completed", result)
            text += "".join(item["text"] for item in result["data"]["segments"])
            cursor = result["data"]["nextCursor"]
        self.assertEqual(text, "\x01" * 900)

    def test_manifest_has_only_read_commands_and_matching_skill_dependencies(self):
        package = ROOT / "plugins" / "bilibili"
        tool = json.loads((package / "tool.json").read_text(encoding="utf-8"))
        skill = json.loads((package / "skill.json").read_text(encoding="utf-8"))
        operations = tool["definition"]["operations"]
        self.assertTrue(adapter.OPERATIONS.issubset({item["id"] for item in operations}))
        declared_dependencies = {operation for dependency in skill["dependencies"] for operation in dependency["operations"]}
        self.assertTrue(READ_OPERATIONS.issubset(declared_dependencies))
        for item in operations:
            self.assertEqual(item["invocation"]["arguments"][0], "-B")
            self.assertFalse(item["parameters"]["schema"]["additionalProperties"])
            if item["id"] in READ_OPERATIONS:
                self.assertEqual(item["effects"]["kind"], "read")

    def test_same_content_reuses_snapshot_and_does_not_store_credentials(self):
        Path(self.temp.name, "credential.json").write_text('{"SESSDATA":"private-value"}', encoding="utf-8")
        self.first_cursor()
        before = sorted(p.name for p in Path(self.temp.name, "transcripts").iterdir())
        self.first_cursor()
        self.assertEqual(before, sorted(p.name for p in Path(self.temp.name, "transcripts").iterdir()))
        for path in Path(self.temp.name, "transcripts").iterdir():
            self.assertNotIn("private-value", path.read_text(encoding="utf-8"))

    def test_local_asr_reads_published_snapshot_without_network(self):
        settings = adapter.validate_settings(self.settings)
        result = self.run_op("read_transcript", {"bvid": BV, "part": 2, "source": "local_asr"})
        self.assertEqual(result["data"]["reason"], "transcription_not_ready")
        snapshot = adapter.save_transcript(settings, self.backend.info, "local_asr",
                                           [{"startMs": 0, "endMs": 1000, "text": "转写"}],
                                           language="zh")
        self.assertRegex(snapshot, r"^[a-f0-9]{64}$")
        result = self.run_op("read_transcript", {"bvid": BV, "part": 2, "source": "local_asr"})
        self.assertEqual(result["data"]["segments"][0]["text"], "转写")
        self.assertEqual(result["data"]["source"], "local_asr")
        self.assertEqual(self.backend.calls, [])

    def test_public_text_is_bounded_and_marks_truncation(self):
        self.backend.info.update(title="🙂" * 600, description="详" * 4000, author="a" * 300)
        result = self.run_op("video_info", {"bvid": BV})
        self.assertTrue(result["data"]["truncated"])
        self.assert_budget(result["data"])
        self.assertLessEqual(utf16(json.dumps(result["data"], ensure_ascii=False)), 3800)
        async def huge(*args):
            return {"entries": [{"bvid": BV, "title": "🙂" * 600, "author": "x" * 500,
                                  "description": '"\\' * 900, "duration": "01:00"}] * 5,
                    "total": 100, "hasMore": True}
        self.backend.search = huge
        result = self.run_op("search", {"keyword": "x"})
        self.assertEqual(len(result["data"]["entries"]), 5)
        self.assertTrue(result["data"]["truncated"])
        self.assertLessEqual(utf16(json.dumps(result["data"], ensure_ascii=False, separators=(",", ":"))), 3800)

    def test_cli_unicode_json_and_no_package_bytecode(self):
        settings_path = Path(self.temp.name, "settings.json")
        settings_path.write_text(json.dumps(self.settings), encoding="utf-8")
        code = ("import sys; sys.dont_write_bytecode=True; "
                "sys.path[:0]=[sys.argv.pop(1),sys.argv.pop(1)]; "
                "from test_bilibili_adapter import FakeBackend; "
                "from plugins.bilibili.adapter import main; "
                "main(backend=FakeBackend())")
        before = list((ROOT / "plugins" / "bilibili").rglob("*.pyc"))
        completed = subprocess.run([sys.executable, "-B", "-c", code, str(ROOT / "tests"), str(ROOT),
                                    "--settings", str(settings_path), "--operation", "search"],
                                   input=json.dumps({"keyword": "中文🙂"}, ensure_ascii=False).encode("utf-8"),
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5,
                                   env=dict(os.environ, PYTHONIOENCODING="ascii"))
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(json.loads(completed.stdout)["data"]["entries"][0]["title"], "中文🙂")
        self.assertEqual(before, list((ROOT / "plugins" / "bilibili").rglob("*.pyc")))

    def test_cli_malformed_json_is_a_failed_envelope(self):
        settings_path = Path(self.temp.name, "settings.json")
        settings_path.write_text(json.dumps(self.settings), encoding="utf-8")
        completed = subprocess.run([sys.executable, "-B", str(ROOT / "plugins" / "bilibili" / "adapter.py"),
                                    "--settings", str(settings_path), "--operation", "status"],
                                   input=b"[1]", stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
        self.assertEqual(completed.returncode, 0)
        self.assertEqual(json.loads(completed.stdout)["data"]["error"]["code"], "INVALID_INPUT")


class BackendTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.settings = adapter.validate_settings({"stateDir": self.temp.name})
        self.calls = []
        owner = self
        class Video:
            def __init__(self, bvid, credential=None):
                owner.calls.append(("Video", bvid))
            async def get_info(self):
                return {"bvid": BV, "aid": 12, "cid": 101, "title": "标题", "desc": "简介",
                        "owner": {"name": "作者"}, "pages": [
                            {"cid": 101, "page": 1, "part": "第一集", "duration": 20},
                            {"cid": 202, "page": 2, "part": "第二集", "duration": 40}]}
            async def get_subtitle(self, cid=None):
                owner.calls.append(("get_subtitle", cid))
                return {"subtitles": [{"lan": "en", "subtitle_url": "https://i0.hdslb.com/en"},
                                       {"lan": "ai-zh", "subtitle_url": "//i0.hdslb.com/zh"}]}
        self.library = SimpleNamespace(video=SimpleNamespace(Video=Video), credential=None)

    def test_backend_second_part_cid_and_duration(self):
        with patch.object(backend, "load_library", return_value=self.library):
            value = asyncio.run(backend.BilibiliBackend(self.settings).video_info(BV, 2))
        self.assertEqual((value["cid"], value["durationSeconds"], value["partTitle"]), (202, 40, "第二集"))

    def test_backend_unknown_part_is_not_first_part(self):
        with patch.object(backend, "load_library", return_value=self.library):
            with self.assertRaises(backend.BilibiliError) as error:
                asyncio.run(backend.BilibiliBackend(self.settings).video_info(BV, 3))
        self.assertEqual(error.exception.code, "INVALID_INPUT")

    def test_subtitle_uses_selected_cid_and_chinese_timestamps(self):
        async def download(_self, url):
            self.assertEqual(url, "https://i0.hdslb.com/zh")
            return {"body": [{"from": 1.25, "to": 4.5, "content": "字幕🙂"}]}
        with patch.object(backend, "load_library", return_value=self.library), \
                patch.object(backend.BilibiliBackend, "download_subtitle", download):
            value = asyncio.run(backend.BilibiliBackend(self.settings).transcript(BV, 2))
        self.assertIn(("get_subtitle", 202), self.calls)
        self.assertEqual(value["segments"], [{"startMs": 1250, "endMs": 4500, "text": "字幕🙂"}])

    def test_platform_error_classification_does_not_echo_sensitive_message(self):
        for code, expected in ((-101, "AUTH_REQUIRED"), (-412, "RATE_LIMITED"), (62002, "PLATFORM_ERROR")):
            error = Exception("SESSDATA=very-secret signed-url")
            error.code = code
            mapped = backend.translate_error(error)
            self.assertEqual(mapped.code, expected)
            self.assertNotIn("secret", mapped.message)

    def test_credential_requirement_is_not_an_empty_transcript(self):
        class CredentialNoSessdataException(Exception):
            pass
        error = backend.translate_error(CredentialNoSessdataException("SESSDATA secret"))
        self.assertEqual(error.code, "AUTH_REQUIRED")

    def test_search_passes_page_size_and_order_and_removes_html(self):
        async def search_by_type(**kwargs):
            self.assertEqual(kwargs, {"keyword": "测试", "search_type": "video", "order_type": "latest",
                                      "page": 2, "page_size": 2})
            return {"result": [{"bvid": BV, "title": '<em class="keyword">测试</em> &amp; 一',
                                 "author": "作者", "duration": "3:00", "description": "简介"}],
                    "numResults": 30, "numPages": 15}
        self.library.search = SimpleNamespace(search_by_type=search_by_type,
            SearchObjectType=SimpleNamespace(VIDEO="video"),
            OrderVideo=SimpleNamespace(TOTALRANK="rank", CLICK="click", PUBDATE="latest", DM="dm"))
        with patch.object(backend, "load_library", return_value=self.library):
            value = asyncio.run(backend.BilibiliBackend(self.settings).search("测试", 2, 2, "pubdate"))
        self.assertEqual(value["entries"][0]["title"], "测试 & 一")
        self.assertTrue(value["hasMore"])

    def test_comments_page_count_controls_more_not_short_reply_list(self):
        async def get_comments(**kwargs):
            self.assertEqual(kwargs, {"oid": 12, "type_": "video", "page_index": 1,
                                      "order": "like", "credential": None})
            return {"page": {"num": 1, "size": 20, "count": 100}, "replies": [
                {"rpid": 123, "member": {"mid": 45, "uname": "作者"}, "content": {"message": "评论"}}]}
        self.library.comment = SimpleNamespace(get_comments=get_comments,
            CommentResourceType=SimpleNamespace(VIDEO="video"), OrderType=SimpleNamespace(LIKE="like"))
        with patch.object(backend, "load_library", return_value=self.library):
            value = asyncio.run(backend.BilibiliBackend(self.settings).comments(BV, 1, 3))
        self.assertTrue(value["hasMore"])
        self.assertEqual(value["entries"][0]["id"], "123")

    def test_subtitle_download_accumulates_network_chunks(self):
        content = json.dumps({"body": [{"from": 0, "to": 1, "content": "字幕🙂"}]}).encode("utf-8")
        chunks = [content[:10], content[10:25], content[25:], b""]
        class Response:
            content = None
            async def __aenter__(self):
                return self
            async def __aexit__(self, *args):
                pass
            def raise_for_status(self):
                pass
            async def read(self, size):
                return chunks.pop(0)
        response = Response()
        response.content = response
        class Session:
            def __init__(self, **kwargs):
                pass
            async def __aenter__(self):
                return self
            async def __aexit__(self, *args):
                pass
            def get(self, url):
                return response
        fake = SimpleNamespace(ClientSession=Session, ClientTimeout=lambda **kwargs: None)
        with patch.dict(sys.modules, {"aiohttp": fake}):
            result = asyncio.run(backend.BilibiliBackend(self.settings).download_subtitle("https://i0.hdslb.com/sub"))
        self.assertEqual(result["body"][0]["content"], "字幕🙂")


if __name__ == "__main__":
    unittest.main()
