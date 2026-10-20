"""Single-attempt QQ delivery and durable receipts against local OneBot only."""
import sys
sys.dont_write_bytecode = True

import json
from pathlib import Path
import sqlite3
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests" / "fixtures"))
from qq_backend import FakeOneBot, message
from plugins.qq import adapter
from plugins.qq.journal import Journal


class QQDeliveryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="qq delivery 中文 ")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.settings = {"backend": "napcat", "wsUrl": "ws://127.0.0.1:1", "selfId": "999",
                         "connectionKey": "pet-qq", "stateDir": str(self.directory / "state"),
                         "allowedConversations": ["group:123", "private:456", "group:456"],
                         "requestTimeoutSeconds": 1}

    def op(self, operation, arguments):
        return adapter.execute(self.settings, operation, arguments)

    def send(self, chat="group:123", text="你好🙂", **arguments):
        return self.op("send", dict(chat_id=chat, text=text, **arguments))

    def receipt(self, sent, chat="group:123"):
        return self.op("receipt", {"chat_id": chat, "receipt_id": sent["data"]["receipt_id"]})

    def connect(self, **kwargs):
        backend = FakeOneBot(**kwargs).__enter__()
        self.addCleanup(backend.__exit__)
        self.settings["wsUrl"] = backend.url
        return backend

    @staticmethod
    def actions(backend, prefix="send_"):
        return [call for call in backend.calls if call["action"].startswith(prefix)]

    def test_send_private_and_group_once(self):
        backend = self.connect()
        for chat, action in (("private:456", "send_private_msg"), ("group:123", "send_group_msg")):
            with self.subTest(chat=chat):
                result = self.send(chat)
                self.assertEqual(result["state"], "completed")
                self.assertEqual(result["data"]["delivery"], "completed")
                self.assertEqual(result["data"]["content"], "你好🙂")
                self.assertTrue(result["data"]["occurred_at"].endswith("Z"))
                self.assertTrue(result["data"]["remote_message_id"].startswith("-"))
                self.assertEqual(result["receipt"], result["data"]["receipt_id"])
                calls = len(backend.calls)
                for _ in range(2):
                    self.assertEqual(self.receipt(result, chat)["data"], result["data"])
                self.assertEqual(calls, len(backend.calls), "Completed receipts need no network")
                self.assertEqual(len(self.actions(backend, action)), 1)
        self.assertEqual(self.actions(backend, "get_friend_msg_history")[0]["params"], {"user_id": 456, "count": 50})

    def test_send_snowluma_confirmed_seq(self):
        self.settings["backend"] = "snowluma"
        backend = self.connect()
        result = self.send()
        self.assertEqual(result["state"], "completed")
        self.assertEqual(self.actions(backend)[0]["action"], "send_msg")
        self.assertEqual(self.actions(backend)[0]["params"]["message_type"], "group")

    def test_snowluma_zero_or_invalid_seq_unknown(self):
        self.settings["backend"] = "snowluma"
        for seq in (0, -1, True, "1", None):
            with self.subTest(seq=seq), FakeOneBot(delivery={"overrides": {"message_seq": seq}}) as backend:
                self.settings["wsUrl"] = backend.url
                self.assertEqual(self.send()["state"], "unknown")
                self.assertEqual(len(self.actions(backend)), 1)

    def test_send_reply_and_literal_cq(self):
        with Journal(self.settings) as journal:
            journal.ingest(message("known"))
        backend = self.connect()
        result = self.send(text="[CQ:at,qq=all] 这只是文本", reply_to="known")
        self.assertEqual(result["state"], "completed")
        segments = self.actions(backend)[0]["params"]["message"]
        self.assertEqual(segments, [{"type": "reply", "data": {"id": "known"}},
                                    {"type": "text", "data": {"text": "[CQ:at,qq=all] 这只是文本"}}])
        self.assertEqual(result["data"]["content"], "[CQ:at,qq=all] 这只是文本")

    def test_actual_remote_body_and_time_in_receipt(self):
        self.connect(delivery={"overrides": {"message": [{"type": "text", "data": {"text": "实际远端正文"}}],
                                                   "time": 1700000000}})
        result = self.send()
        self.assertEqual(result["state"], "completed")
        self.assertEqual(result["data"]["content"], "实际远端正文")
        self.assertEqual(result["data"]["occurred_at"], "2023-11-14T22:13:20.000Z")

    def test_confirm_wrong_target_sender_time_unknown(self):
        changes = [{"group_id": 777}, {"sender": {"user_id": 123}}, {"time": None},
                   {"time": True}, {"message_id": "another"}, {"message_type": "private"},
                   {"message": []}, {"self_id": 111}]
        for delta in changes:
            with self.subTest(delta=delta), FakeOneBot(delivery={"overrides": delta}) as backend:
                self.settings["wsUrl"] = backend.url
                sent = self.send()
                self.assertEqual(sent["state"], "unknown")
                self.assertEqual(sent["data"]["delivery"], "unknown")
                self.assertNotIn("content", sent["data"])
                self.assertEqual(sent["tracking"]["operation"], "receipt")
                result = self.receipt(sent)
                self.assertEqual(result["state"], "completed")
                self.assertEqual(result["data"]["delivery"], "unknown")
                self.assertEqual(len(self.actions(backend)), 1)

    def test_private_explicit_target_needs_no_history(self):
        backend = self.connect(delivery={"overrides": {"target_id": 456}}, unsupported=["get_friend_msg_history"])
        self.assertEqual(self.send("private:456")["state"], "completed")
        self.assertEqual(self.actions(backend, "get_friend_msg_history"), [])

    def test_private_wrong_target_never_overridden_by_history(self):
        backend = self.connect(delivery={"overrides": {"target_id": 777}})
        self.assertEqual(self.send("private:456")["state"], "unknown")
        self.assertEqual(self.actions(backend, "get_friend_msg_history"), [])

    def test_private_history_missing_or_unsupported_unknown(self):
        for kwargs in ({"delivery": {"omit_history": True}}, {"unsupported": ["get_friend_msg_history"]}):
            with self.subTest(kwargs=kwargs), FakeOneBot(**kwargs) as backend:
                self.settings["wsUrl"] = backend.url
                self.assertEqual(self.send("private:456")["state"], "unknown")
                self.assertEqual(len(self.actions(backend, "get_friend_msg_history")), 1)

    def test_private_history_must_identify_same_valid_message(self):
        backend = self.connect(delivery={"omit_history": True})
        def history(ws, request):
            if request["action"] != "get_friend_msg_history":
                return False
            wrong = message("-1001", "private:456", actor="111")
            ws.send(json.dumps({"echo": request["echo"], "status": "ok", "retcode": 0,
                                "data": {"messages": [wrong]}}))
            return True
        backend.hook = history
        self.assertEqual(self.send("private:456")["state"], "unknown")

    def test_get_msg_unsupported_unknown(self):
        backend = self.connect(unsupported=["get_msg"])
        result = self.send()
        self.assertEqual(result["state"], "unknown")
        self.assertIn("remote_message_id", result["data"])
        self.assertEqual(len(self.actions(backend)), 1)

    def test_send_timeout_no_retry(self):
        backend = self.connect()
        backend.hook = lambda ws, request: request["action"].startswith("send_")
        result = self.send()
        self.assertEqual(result["state"], "unknown")
        self.assertNotIn("remote_message_id", result["data"])
        calls = len(backend.calls)
        self.assertEqual(self.receipt(result)["data"]["delivery"], "unknown")
        self.assertEqual(calls, len(backend.calls))
        self.assertEqual(len(self.actions(backend)), 1)

    def test_send_disconnect_no_retry(self):
        backend = self.connect()
        def disconnect(ws, request):
            if request["action"].startswith("send_"):
                ws.close()
                return True
            return False
        backend.hook = disconnect
        result = self.send()
        self.assertEqual(result["state"], "unknown")
        self.assertEqual(self.receipt(result)["data"]["delivery"], "unknown")
        self.assertEqual(len(self.actions(backend)), 1)

    def test_send_explicit_rejection_failed(self):
        backend = self.connect(unsupported=["send_group_msg"])
        result = self.send()
        self.assertEqual(result["state"], "failed")
        self.assertEqual(result["data"]["delivery"], "failed")
        self.assertEqual(result["data"]["error"]["code"], "UNSUPPORTED_ACTION")
        self.assertEqual(self.receipt(result)["data"]["delivery"], "failed")
        self.assertEqual(len(self.actions(backend)), 1)

    def test_send_ambiguous_response_unknown(self):
        for fields in ({}, {"status": "async", "retcode": 1}, {"status": "ok", "retcode": True}):
            with self.subTest(fields=fields), FakeOneBot() as backend:
                self.settings["wsUrl"] = backend.url
                def ambiguous(ws, request):
                    if not request["action"].startswith("send_"):
                        return False
                    ws.send(json.dumps(dict(fields, echo=request["echo"])))
                    return True
                backend.hook = ambiguous
                self.assertEqual(self.send()["state"], "unknown")
                self.assertEqual(len(self.actions(backend)), 1)

    def test_receipt_after_restart(self):
        backend = self.connect(unsupported=["get_msg"])
        sent = self.send("private:456")
        self.assertEqual(sent["state"], "unknown")
        backend.unsupported.clear()
        result = self.cli("receipt", {"chat_id": "private:456", "receipt_id": sent["data"]["receipt_id"]})
        self.assertEqual(result["state"], "completed")
        self.assertEqual(result["data"]["delivery"], "completed")
        self.assertEqual(len(self.actions(backend)), 1)
        cached = self.cli("receipt", {"chat_id": "private:456", "receipt_id": sent["data"]["receipt_id"]})
        self.assertEqual(cached, result)

    def test_receipt_no_remote_id_unknown(self):
        backend = self.connect(delivery={"omit_message_id": True})
        sent = self.send()
        self.assertEqual(sent["state"], "unknown")
        count = len(backend.calls)
        self.assertEqual(self.receipt(sent)["data"]["delivery"], "unknown")
        self.assertEqual(count, len(backend.calls))
        self.assertEqual(len(self.actions(backend)), 1)

    def test_pending_attempt_after_crash_no_resend(self):
        backend = self.connect()
        with Journal(self.settings) as journal:
            pending = journal.create_attempt("group:123", "先前尝试", "")
        result = self.op("receipt", {"chat_id": "group:123", "receipt_id": pending["receipt_id"]})
        self.assertEqual(result["state"], "completed")
        self.assertEqual(result["data"]["delivery"], "unknown")
        self.assertEqual(backend.calls, [])

    def test_send_denied_no_network(self):
        backend = self.connect()
        with Journal(self.settings) as journal:
            journal.ingest(message("other-room", "group:456"))
        cases = [("group:777", "正文", {}), ("group:123", "", {}),
                 ("group:123", "🙂" * 4001, {}), ("group:123", "bad\ud800", {}),
                 ("group:123", "正文", {"reply_to": "other-room"}),
                 ("group:123", "正文", {"reply_to": 123}),
                 ("group:123", "正文", {"unexpected": True})]
        for chat, text, extra in cases:
            with self.subTest(chat=chat, extra=extra):
                self.assertEqual(self.send(chat, text, **extra)["state"], "failed")
        self.assertEqual(backend.calls, [])

    def test_wrong_account_blocks_send(self):
        backend = self.connect(self_id="888")
        result = self.send()
        self.assertEqual(result["state"], "failed")
        self.assertEqual(result["data"]["error"]["code"], "ACCOUNT_MISMATCH")
        self.assertEqual(self.actions(backend), [])

    def test_receipt_scope_rejected(self):
        backend = self.connect()
        sent = self.send()
        calls = len(backend.calls)
        self.assertEqual(self.receipt(sent, "private:456")["state"], "failed")
        self.assertEqual(self.receipt(sent, "group:777")["state"], "failed")
        self.assertEqual(self.op("receipt", {"chat_id": "group:123", "receipt_id": "missing"})["state"], "failed")
        self.assertEqual(calls, len(backend.calls))

    def test_pending_storage_failure_prevents_send(self):
        backend = self.connect()
        with patch.object(Journal, "create_attempt", side_effect=sqlite3.OperationalError("disk full")):
            result = self.send()
        self.assertEqual(result["state"], "failed")
        self.assertEqual(result["data"]["error"]["code"], "STORAGE_ERROR")
        self.assertEqual(self.actions(backend), [])

    def test_post_dispatch_storage_failure_keeps_unknown_receipt(self):
        backend = self.connect()
        with patch.object(Journal, "update_attempt", side_effect=sqlite3.OperationalError("disk full")):
            result = self.send()
        self.assertEqual(result["state"], "unknown")
        self.assertTrue(result["data"]["receipt_id"])
        self.assertEqual(result["data"]["remote_message_id"], "-1001")
        self.assertEqual(self.receipt(result)["data"]["delivery"], "unknown")
        self.assertEqual(len(self.actions(backend)), 1)

    def test_cli_send_and_fixture_process(self):
        ready = self.directory / "ready.json"
        log = self.directory / "actions.jsonl"
        process = subprocess.Popen([sys.executable, "-B", str(ROOT / "tests" / "fixtures" / "qq_backend.py"),
                                    "--ready-file", str(ready), "--action-log", str(log)],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(self.stop_process, process)
        import time
        until = time.monotonic() + 5
        while not ready.exists() and time.monotonic() < until:
            time.sleep(.02)
        self.assertTrue(ready.exists(), "Local fixture did not start")
        self.settings["wsUrl"] = json.loads(ready.read_text())["wsUrl"]
        sent = self.cli("send", {"chat_id": "private:456", "text": "实际命令收发🙂"})
        self.assertEqual(sent["state"], "completed")
        result = self.cli("receipt", {"chat_id": "private:456", "receipt_id": sent["data"]["receipt_id"]})
        self.assertEqual(result["data"]["content"], "实际命令收发🙂")
        self.assertEqual(sum(json.loads(line)["action"].startswith("send_") for line in log.read_text().splitlines()), 1)
        self.assertEqual(list((ROOT / "plugins" / "qq").rglob("__pycache__")), [])

    @staticmethod
    def stop_process(process):
        if process.poll() is None:
            process.terminate()
        process.wait(timeout=5)
        process.stdout.close()
        process.stderr.close()

    def cli(self, operation, arguments):
        settings = self.directory / "settings.json"
        settings.write_text(json.dumps(self.settings), encoding="utf-8")
        process = subprocess.run([sys.executable, "-B", str(ROOT / "plugins" / "qq" / "adapter.py"),
                                  "--settings", str(settings), "--operation", operation],
                                 input=json.dumps(arguments).encode(), stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertEqual(process.stderr, b"")
        return json.loads(process.stdout)


if __name__ == "__main__":
    unittest.main()
