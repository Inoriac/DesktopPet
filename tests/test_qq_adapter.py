"""QQ reads and durable observations tested against a local OneBot server."""
import sys
sys.dont_write_bytecode = True

import json
from pathlib import Path
import queue
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests" / "fixtures"))
from qq_backend import FakeOneBot, message
from plugins.qq import adapter, common
from plugins.qq.journal import Journal


class QQAdapterTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="qq adapter 中文 ")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.settings = {"backend": "napcat", "wsUrl": "ws://127.0.0.1:1", "selfId": "999",
                         "connectionKey": "pet-qq", "stateDir": str(self.directory / "state"),
                         "allowedConversations": ["group:123", "private:123", "private:456"],
                         "requestTimeoutSeconds": 1}

    def op(self, operation, arguments=None):
        return adapter.execute(self.settings, operation, arguments or {})

    def journal(self):
        result = Journal(self.settings)
        self.addCleanup(result.close)
        return result

    def start_source(self, cursor=""):
        path = self.directory / "settings.json"
        path.write_text(json.dumps(self.settings), encoding="utf-8")
        process = subprocess.Popen([sys.executable, "-B", str(ROOT / "plugins" / "qq" / "source.py"),
                                    "--settings", str(path)], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        lines = queue.Queue()
        def reader():
            for line in process.stdout:
                lines.put(line)
        thread = threading.Thread(target=reader, daemon=True)
        thread.start()
        def cleanup():
            if process.poll() is None:
                process.kill()
            process.wait(timeout=5)
            for stream in (process.stdin, process.stdout, process.stderr):
                stream.close()
            thread.join(timeout=1)
        self.addCleanup(cleanup)
        self.control(process, {"type": "start", "cursor": cursor})
        return process, lines

    @staticmethod
    def control(process, value):
        process.stdin.write((json.dumps(value) + "\n").encode())
        process.stdin.flush()

    @staticmethod
    def wait_until(predicate, timeout=5):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if predicate():
                return
            time.sleep(.02)
        raise AssertionError("Timed out waiting for controlled backend state")

    def test_read_authorized_history(self):
        history = {"group:123": [message("new", timestamp=1770000002), message("old", timestamp=1770000001),
                                   message("wrong", "group:777")],
                   "private:123": [message("old", "private:123", actor="123")]}
        with FakeOneBot(history=history) as backend:
            self.settings["wsUrl"] = backend.url
            read = self.op("read", {"chat_id": "group:123", "limit": 1})
            self.assertEqual(read["state"], "completed")
            self.assertEqual(read["data"]["freshness"], "remote")
            self.assertEqual([x["message_id"] for x in read["data"]["items"]], ["new"])
            cursor = read["data"]["next_cursor"]
            previous = self.op("read", {"chat_id": "group:123", "cursor": cursor})
            self.assertEqual([x["message_id"] for x in previous["data"]["items"]], ["old"])
            self.assertEqual(previous["data"]["freshness"], "local_page")
            private = self.op("read", {"chat_id": "private:123"})["data"]["items"]
            self.assertNotEqual(private[0]["event_id"], previous["data"]["items"][0]["event_id"])
            calls = len(backend.calls)
            denied = self.op("read", {"chat_id": "group:777"})
            self.assertEqual(denied["data"]["error"]["code"], "PERMISSION_DENIED")
            self.assertEqual(calls, len(backend.calls))
            self.assertEqual([c["action"] for c in backend.calls].count("get_group_msg_history"), 1)

    def test_normalize_private_with_group_id(self):
        incoming = message("temp", "private:456")
        incoming["group_id"] = 123
        item = self.journal().ingest(incoming)
        self.assertEqual(item["chat_id"], "private:456")
        self.assertEqual(item["speaker_id"], "456")

    def test_history_self_private_without_target_and_negative_message_id(self):
        incoming = message(-123, "private:456", actor="999")
        del incoming["target_id"]
        journal = self.journal()
        self.assertIsNone(journal.ingest(incoming))
        item = journal.ingest(incoming, historical=True, requested_chat="private:456")
        self.assertEqual(item["chat_id"], "private:456")
        self.assertEqual(item["message_id"], "-123")
        self.assertEqual(item["speaker_id"], "999")
        self.assertTrue(item["historical"])

    def test_normalize_media_missing_time(self):
        incoming = message(timestamp=None)
        incoming["message"] = [{"type": "reply", "data": {"id": "old"}},
                               {"type": "at", "data": {"qq": "999"}},
                               {"type": "text", "data": {"text": "中文🙂"}},
                               {"type": "image", "data": {"url": "secret"}},
                               {"type": "record", "data": {}}, {"type": "face", "data": {"id": "14"}}]
        item = self.journal().ingest(incoming)
        self.assertEqual(item["reply_to"], "old")
        self.assertNotIn("occurred_at", item)
        self.assertIn("中文🙂", item["text"])
        self.assertIn("图片", item["text"])
        self.assertIn("语音", item["text"])
        self.assertIn("14", item["text"])
        self.assertNotIn("secret", item["text"])
        incoming["message_id"] = "cq"
        incoming["message"] = "a&#91;b&#93; [CQ:at,qq=all] [CQ:reply,id=old] [CQ:image,file=private]"
        item = self.journal().ingest(incoming)
        self.assertIn("a[b]", item["text"])
        self.assertIn("@所有人", item["text"])
        self.assertEqual(item["reply_to"], "old")
        long = self.journal().ingest(message("long", text="🙂" * 8000))
        self.assertLessEqual(len(long["text"].encode("utf-16-le")) // 2, 8000)
        self.assertIn("截断", long["text"])
        malformed = message("malformed")
        malformed["message"].insert(0, {"type": {}, "data": {}})
        self.assertEqual(self.journal().ingest(malformed)["text"], "你好🙂")

    def test_journal_replay_dedupe(self):
        journal = self.journal()
        original = journal.ingest(message())
        changed = message(text="重新查询时变化的正文", timestamp=None)
        self.assertEqual(journal.ingest(changed, historical=True), original)
        self.assertEqual(self.journal().after(""), [original])
        self.assertFalse(original["historical"])
        with self.assertRaises(common.AdapterError):
            journal.after("qq:another-database:1")
        with self.assertRaises(common.AdapterError):
            journal.read("private:456", original["cursor"])
        mismatched = dict(self.settings, selfId="888")
        with self.assertRaises(common.AdapterError) as error:
            Journal(mismatched)
        self.assertEqual(error.exception.code, "ACCOUNT_MISMATCH")

    def test_query_does_not_ack(self):
        journal = self.journal()
        value = journal.ingest(message())
        journal.read("group:123")
        self.assertEqual(journal.db.execute("SELECT COUNT(*) FROM source_ack").fetchone()[0], 0)
        journal.ack(value["event_id"], value["cursor"])
        journal.read("group:123")
        self.assertEqual(journal.db.execute("SELECT cursor FROM source_ack").fetchone()[0], value["cursor"])

    def test_history_and_live_source_preserve_first_committed_payload(self):
        changed = message(text="历史接口不同的展示正文", timestamp=None)
        with FakeOneBot(history={"group:123": [changed]}, events=[message()]) as backend:
            self.settings["wsUrl"] = backend.url
            process, lines = self.start_source()
            event = json.loads(lines.get(timeout=5))
            value = self.op("read", {"chat_id": "group:123"})["data"]["items"][0]
            self.assertEqual(value["event_id"], event["eventId"])
            self.assertEqual(value["text"], event["message"]["text"])
            self.assertEqual(value["occurred_at"], event["occurredAt"])
            self.assertFalse(value["historical"])
            self.assertEqual(self.journal().db.execute("SELECT COUNT(*) FROM source_ack").fetchone()[0], 0)
            self.control(process, {"type": "stop"})
            self.assertEqual(process.wait(timeout=3), 0)

    def test_source_process_replay_ack_stop(self):
        journal = self.journal()
        value = journal.ingest(message(timestamp=None))
        process, lines = self.start_source()
        event = json.loads(lines.get(timeout=5))
        self.assertEqual(event["eventId"], value["event_id"])
        self.assertEqual(event["speaker"]["namespace"], "pet-qq")
        self.assertEqual(event["conversation"], {"remoteConversationId": "group:123", "kind": "group"})
        self.assertNotIn("occurredAt", event)
        self.control(process, {"type": "ack", "eventId": event["eventId"], "cursor": event["cursor"]})
        self.wait_until(lambda: journal.db.execute("SELECT COUNT(*) FROM source_ack").fetchone()[0] == 1)
        self.control(process, {"type": "stop"})
        self.assertEqual(process.wait(timeout=3), 0)
        resumed, output = self.start_source(value["cursor"])
        with self.assertRaises(queue.Empty):
            output.get(timeout=.3)
        self.control(resumed, {"type": "stop"})
        self.assertEqual(resumed.wait(timeout=3), 0)

    def test_source_backpressure_eof(self):
        journal = self.journal()
        for i in range(66):
            journal.ingest(message(str(i)))
        process, lines = self.start_source()
        first = [json.loads(lines.get(timeout=5)) for _ in range(64)]
        with self.assertRaises(queue.Empty):
            lines.get(timeout=.3)
        self.control(process, {"type": "ack", "eventId": first[0]["eventId"], "cursor": first[0]["cursor"]})
        self.assertEqual(json.loads(lines.get(timeout=3))["sequence"], "65")
        with self.assertRaises(queue.Empty):
            lines.get(timeout=.2)
        process.stdin.close()
        self.assertEqual(process.wait(timeout=3), 0)

    def test_source_backpressure_keeps_ingesting_to_disk(self):
        with FakeOneBot(events=[message(str(i)) for i in range(80)]) as backend:
            self.settings["wsUrl"] = backend.url
            process, lines = self.start_source()
            for _ in range(64):
                lines.get(timeout=5)
            journal = self.journal()
            self.wait_until(lambda: journal.db.execute("SELECT COUNT(*) FROM messages").fetchone()[0] == 80)
            with self.assertRaises(queue.Empty):
                lines.get(timeout=.2)
            self.control(process, {"type": "stop"})
            self.assertEqual(process.wait(timeout=3), 0)

    def test_wrong_account_rejected(self):
        with FakeOneBot(self_id="888", events=[message()]) as backend:
            self.settings["wsUrl"] = backend.url
            result = self.op("read", {"chat_id": "group:123"})
            self.assertEqual(result["data"]["error"]["code"], "ACCOUNT_MISMATCH")
            self.assertEqual([c["action"] for c in backend.calls], ["get_login_info"])
            process, lines = self.start_source()
            self.assertNotEqual(process.wait(timeout=5), 0)
            self.assertTrue(lines.empty())
            self.assertEqual(self.journal().after(""), [])

    def test_history_unsupported_local_only(self):
        original = self.journal().ingest(message())
        with FakeOneBot(unsupported=["get_group_msg_history"]) as backend:
            self.settings["wsUrl"] = backend.url
            value = self.op("read", {"chat_id": "group:123"})
            self.assertEqual(value["state"], "completed")
            self.assertEqual(value["data"]["freshness"], "local_only")
            self.assertIn("warning", value["data"])
            self.assertEqual(value["data"]["items"], [original])

    def test_source_reconnect(self):
        with FakeOneBot(events=[message(), message("denied", "group:777")]) as backend:
            self.settings["wsUrl"] = backend.url
            process, lines = self.start_source()
            event = json.loads(lines.get(timeout=5))
            self.control(process, {"type": "ack", "eventId": event["eventId"], "cursor": event["cursor"]})
            backend.disconnect()
            self.wait_until(lambda: len(backend.calls) >= 2)
            backend.emit(message("m2", "private:456"))
            next_event = json.loads(lines.get(timeout=5))
            self.assertEqual(next_event["sequence"], "2")
            self.assertEqual(next_event["conversation"]["kind"], "direct")
            self.control(process, {"type": "stop"})
            self.assertEqual(process.wait(timeout=3), 0)

    def test_source_prelogin_event_waits_for_account_verification(self):
        for self_id in ("999", "888"):
            with self.subTest(self_id=self_id), FakeOneBot(self_id=self_id) as backend:
                self.settings["wsUrl"] = backend.url
                self.settings["stateDir"] = str(self.directory / ("prelogin-" + self_id))
                def prelogin(ws, request):
                    if request["action"] == "get_login_info":
                        ws.send(json.dumps(message("before-login")))
                    return False
                backend.hook = prelogin
                process, lines = self.start_source()
                if self_id == "888":
                    self.assertNotEqual(process.wait(timeout=5), 0)
                    self.assertTrue(lines.empty())
                    self.assertEqual(self.journal().after(""), [])
                else:
                    event = json.loads(lines.get(timeout=5))
                    self.assertEqual(event["message"]["remoteMessageId"], "before-login")
                    self.control(process, {"type": "stop"})
                    self.assertEqual(process.wait(timeout=3), 0)

    def test_command_ignores_busy_unsolicited_events(self):
        with FakeOneBot() as backend:
            self.settings["wsUrl"] = backend.url
            def flood(ws, request):
                for i in range(80):
                    ws.send(json.dumps(message(str(i))))
                return False
            backend.hook = flood
            self.assertEqual(self.op("status")["state"], "completed")
            self.assertEqual(self.journal().after(""), [])

    def test_action_outcome_requires_unambiguous_status_and_retcode(self):
        cases = [({"status": "async", "retcode": 1}, "PROTOCOL_ERROR"),
                 ({"status": "ok", "retcode": 1}, "PROTOCOL_ERROR"),
                 ({"status": "failed"}, "PROTOCOL_ERROR"),
                 ({"status": "failed", "retcode": 0}, "PROTOCOL_ERROR"),
                 ({"status": "ok", "retcode": False}, "PROTOCOL_ERROR"),
                 ({}, "PROTOCOL_ERROR"),
                 ({"status": "failed", "retcode": 100}, "API_REJECTED"),
                 ({"status": "failed", "retcode": 1404}, "UNSUPPORTED_ACTION")]
        with FakeOneBot() as backend:
            self.settings["wsUrl"] = backend.url
            for reply, expected in cases:
                with self.subTest(reply=reply):
                    def respond(ws, request):
                        ws.send(json.dumps(dict(reply, echo=request["echo"])))
                        return True
                    backend.hook = respond
                    result = self.op("status")
                    self.assertEqual(result["state"], "failed")
                    self.assertEqual(result["data"]["error"]["code"], expected)

    def test_status_and_list_token_and_settings(self):
        token = self.directory / "token.txt"
        token.write_text("test-secret\n", encoding="utf-8")
        self.settings["tokenFile"] = str(token)
        with FakeOneBot() as backend:
            self.settings["wsUrl"] = backend.url
            result = self.op("status")
            self.assertEqual(result["state"], "completed")
            self.assertEqual(result["data"]["self_id"], "999")
            self.assertEqual(backend.headers[0]["authorization"], "Bearer test-secret")
            self.assertNotIn("test-secret", json.dumps(result))
        listing = self.op("list")
        self.assertEqual([x["chat_id"] for x in listing["data"]["conversations"]], self.settings["allowedConversations"])
        self.journal().ingest(message("summary", text="🙂" * 500))
        summary = self.op("list")["data"]["conversations"][0]["last_message"]
        self.assertLessEqual(len(summary["text"].encode("utf-16-le")) // 2, 160)
        self.assertNotIn("event_id", summary)
        for delta in ({"stateDir": str(ROOT / "plugins" / "qq" / "state")},
                      {"wsUrl": "ws://user:secret@localhost"}, {"wsUrl": "ws://localhost?access_token=secret"},
                      {"wsUrl": []},
                      {"allowedConversations": ["group:0"]}):
            with self.subTest(delta=delta), self.assertRaises(common.AdapterError):
                common.validate_settings(dict(self.settings, **delta))

    def test_command_json_and_storage_failure_are_reported(self):
        path = self.directory / "settings.json"
        path.write_text(json.dumps(self.settings), encoding="utf-8")
        command = [sys.executable, "-B", str(ROOT / "plugins" / "qq" / "adapter.py"),
                   "--settings", str(path), "--operation", "list"]
        result = subprocess.run(command, input=b"{}", stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stdout)["state"], "completed")
        invalid = subprocess.run(command, input=b"[]", stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
        self.assertEqual(json.loads(invalid.stdout)["data"]["error"]["code"], "INVALID_INPUT")
        blocked = self.directory / "not-a-directory"
        blocked.write_text("keep me", encoding="utf-8")
        self.settings["stateDir"] = str(blocked)
        self.assertEqual(self.op("list")["data"]["error"]["code"], "STORAGE_ERROR")
        self.assertEqual(blocked.read_text(encoding="utf-8"), "keep me")
        self.assertEqual(list((ROOT / "plugins" / "qq").rglob("__pycache__")), [])


if __name__ == "__main__":
    unittest.main()
