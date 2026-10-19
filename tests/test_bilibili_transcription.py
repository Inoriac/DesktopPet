"""Exercise actual ASR supervisor/worker processes without network or models."""
import concurrent.futures
import asyncio
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests" / "fixtures"))
from plugins.bilibili import adapter, transcription
from bilibili_asr_engine import install

BV = "BV1xx411c7mD"


class Backend:
    def __init__(self, duration=60):
        self.duration = duration
        self.calls = []

    async def video_info(self, bvid, part):
        self.calls.append((bvid, part))
        return {"bvid": bvid, "part": part, "cid": 202 if part == 2 else 101,
                "title": "声音🙂", "durationSeconds": self.duration}


class TranscriptionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="bilibili ASR 中文 ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.state = self.root / "state"
        self.model = self.root / "model"
        self.model.mkdir()
        for name in ("model.bin", "config.json", "tokenizer.json"):
            (self.model / name).write_text("{}", encoding="utf-8")
        self.dependencies = install(self.root / "dependencies")
        self.environment = patch.dict(os.environ, {"PYTHONPATH": str(self.dependencies)})
        self.environment.start()
        self.addCleanup(self.environment.stop)
        self.settings = {"stateDir": str(self.state), "asrEnabled": True,
                         "asrPython": sys.executable, "asrModelPath": str(self.model),
                         "asrJobTimeoutSeconds": 5}
        self.backend = Backend()

    def start(self, part=2):
        return adapter.run("start_transcription", {"bvid": BV, "part": part}, self.settings, self.backend)

    def query(self, job):
        return adapter.run("transcription_status", {"job_id": job}, self.settings)

    def wait(self, job, timeout=8):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            result = self.query(job)
            if result["state"] != "completed" or result["data"]["status"] not in ("queued", "running"):
                return result
            time.sleep(0.03)
        self.fail("Worker did not reach a terminal state")

    def assert_error(self, result, code):
        self.assertEqual(result["state"], "failed", result)
        self.assertEqual(result["data"]["error"]["code"], code, result)

    def test_disabled_missing_python_model_dependencies_do_not_start(self):
        for delta, code in (({"asrEnabled": False}, "ASR_DISABLED"),
                            ({"asrPython": None}, "ASR_NOT_READY"),
                            ({"asrModelPath": str(self.root / "absent")}, "ASR_NOT_READY")):
            with self.subTest(delta=delta):
                result = adapter.run("start_transcription", {"bvid": BV}, dict(self.settings, **delta), self.backend)
                self.assert_error(result, code)
        with patch.object(transcription, "check_runtime", side_effect=adapter.AdapterError("DEPENDENCY_MISSING", "Missing ASR dependencies")):
            self.assert_error(self.start(), "DEPENDENCY_MISSING")
        self.assertEqual(self.backend.calls, [])
        self.assertFalse(self.state.exists())

    def test_partial_model_and_overlong_video_are_rejected(self):
        (self.model / "model.bin").unlink()
        self.assert_error(self.start(), "ASR_NOT_READY")
        (self.model / "model.bin").write_bytes(b"model")
        self.backend.duration = 1801
        self.assert_error(self.start(), "ASR_TOO_LONG")
        self.assertFalse((self.state / "jobs").exists())

    def test_real_dependency_import_failure_has_no_job(self):
        (self.dependencies / "faster_whisper" / "__init__.py").write_text("raise ImportError('not installed')", encoding="utf-8")
        self.assert_error(self.start(), "DEPENDENCY_MISSING")
        self.assertFalse(self.state.exists())

    def test_duration_lookup_obeys_whole_request_deadline(self):
        class SlowBackend:
            async def video_info(self, bvid, part):
                await asyncio.sleep(4)
        self.settings["requestTimeoutSeconds"] = 1
        start = time.monotonic()
        result = adapter.run("start_transcription", {"bvid":BV}, self.settings, SlowBackend())
        self.assert_error(result, "NETWORK_TIMEOUT")
        self.assertLess(time.monotonic() - start, 2)
        self.assertFalse((self.state / "jobs").exists())

    def test_accepted_receipt_deduplicates_and_reads_timed_pages(self):
        text = "中文🙂" * 1400
        install(self.dependencies, sleep=0.35, segments=[{"start":1.25,"end":4.5,"text":text}])
        begin = time.monotonic()
        result = self.start()
        self.assertEqual(result["state"], "accepted", result)
        self.assertLess(time.monotonic() - begin, 2)
        job = result["data"]["jobId"]
        self.assertEqual(result["receipt"]["jobId"], job)
        self.assertEqual(result["tracking"], {"operation":"transcription_status", "arguments":{"job_id":job}})
        second = self.start()
        self.assertEqual(second["receipt"]["jobId"], job)
        done = self.wait(job)
        self.assertEqual(done["data"]["status"], "completed", done)
        self.assertEqual(done["data"]["source"], "local_asr")
        read, cursor = "", None
        while True:
            args = {"bvid":BV, "part":2, "source":"local_asr"}
            if cursor: args["cursor"] = cursor
            page = adapter.run("read_transcript", args, self.settings)
            self.assertEqual(page["state"], "completed", page)
            self.assertLessEqual(adapter.utf16_size(adapter.json_text(page["data"])), 3800)
            for segment in page["data"]["segments"]:
                self.assertEqual((segment["startMs"],segment["endMs"]), (1250,4500))
                read += segment["text"]
            cursor = page["data"]["nextCursor"]
            if not cursor: break
        self.assertEqual(read, text)
        self.assertFalse((self.state / "jobs" / job / "work").exists())
        self.assertEqual(self.start()["state"], "completed")
        self.assertFalse(list((ROOT / "plugins" / "bilibili").rglob("*.pyc")))

    def test_concurrent_acceptance_starts_one_job(self):
        install(self.dependencies, sleep=0.4)
        with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
            results = list(pool.map(lambda _: self.start(), range(3)))
        self.assertTrue(all(r["state"] == "accepted" for r in results), results)
        jobs = {r["receipt"]["jobId"] for r in results}
        self.assertEqual(len(jobs), 1)
        self.assertEqual(self.wait(jobs.pop())["data"]["status"], "completed")

    def test_empty_speech_completes_with_no_speech(self):
        install(self.dependencies, segments=[])
        done = self.wait(self.start()["data"]["jobId"])
        self.assertEqual(done["data"]["status"], "completed", done)
        page = adapter.run("read_transcript", {"bvid":BV,"part":2,"source":"local_asr"}, self.settings)
        self.assertEqual(page["data"]["reason"], "no_speech")
        self.assertEqual(page["data"]["segments"], [])

    def test_download_engine_failure_and_timeout_clean_only_job_audio(self):
        sibling = self.state / "keep-audio.m4a"
        self.state.mkdir()
        sibling.write_bytes(b"keep")
        for behavior, code in (({"downloadError":True}, "ASR_DOWNLOAD_FAILED"),
                               ({"engineError":True}, "ASR_ENGINE_FAILED"),
                               ({"sleep":4}, "ASR_TIMEOUT")):
            install(self.dependencies, **behavior)
            self.settings["asrJobTimeoutSeconds"] = 1
            with self.subTest(behavior=behavior):
                job = self.start()["data"]["jobId"]
                done = self.wait(job)
                self.assertEqual(done["data"]["status"], "failed", done)
                self.assertEqual(done["data"]["error"]["code"], code)
                self.assertNotIn("secret", json.dumps(done))
                self.assertFalse((self.state / "jobs" / job / "work").exists())
                self.assertEqual(sibling.read_bytes(), b"keep")

    def test_wrong_download_part_and_changed_duration_are_not_transcribed(self):
        for behavior, code in (({"wrongPart":True}, "ASR_DOWNLOAD_FAILED"),
                               ({"downloadDuration":2000}, "ASR_TOO_LONG")):
            install(self.dependencies, **behavior)
            with self.subTest(behavior=behavior):
                done = self.wait(self.start()["data"]["jobId"])
                self.assertEqual(done["data"]["status"], "failed", done)
                self.assertEqual(done["data"]["error"]["code"], code)

    def test_account_change_prevents_publication_or_cross_account_status(self):
        self.state.mkdir()
        credential = self.state / "credential.json"
        credential.write_text('{"SESSDATA":"old-secret"}', encoding="utf-8")
        install(self.dependencies, sleep=0.4)
        job = self.start()["data"]["jobId"]
        deadline = time.monotonic() + 3
        while not (self.state / "jobs" / job / "work" / "audio.m4a").exists() and time.monotonic() < deadline:
            time.sleep(0.02)
        credential.write_text('{"SESSDATA":"new-secret"}', encoding="utf-8")
        self.assert_error(self.query(job), "JOB_NOT_FOUND")
        time.sleep(0.6)
        credential.write_text('{"SESSDATA":"old-secret"}', encoding="utf-8")
        done = self.wait(job)
        self.assertEqual(done["data"]["error"]["code"], "CREDENTIAL_CHANGED", done)
        self.assertFalse((self.state / "transcripts").exists())
        for path in (self.state / "jobs").rglob("*.json"):
            self.assertNotIn("old-secret", path.read_text(encoding="utf-8"))

    def test_invalid_unknown_and_abandoned_job_states(self):
        self.assert_error(self.query("../credential.json"), "INVALID_INPUT")
        self.assert_error(self.query("a" * 32), "JOB_NOT_FOUND")
        with patch.object(transcription, "launch_worker"):
            job = self.start()["data"]["jobId"]
        self.assertEqual(self.query(job)["data"]["status"], "queued")
        path = self.state / "jobs" / job / "job.json"
        record = json.loads(path.read_text(encoding="utf-8"))
        record["deadline"] = time.time() - 10
        path.write_text(json.dumps(record), encoding="utf-8")
        done = self.query(job)
        self.assertEqual(done["data"]["status"], "unknown", done)
        self.assertEqual(done["data"]["error"]["code"], "ASR_UNKNOWN")
        self.assert_error(self.start(), "ASR_UNKNOWN")

    def test_launch_failure_has_terminal_state(self):
        with patch.object(transcription, "launch_worker", side_effect=OSError("sensitive path")):
            result = self.start()
        self.assert_error(result, "ASR_START_FAILED")
        records = list((self.state / "jobs").glob("*/job.json"))
        self.assertEqual(len(records), 1)
        self.assertEqual(json.loads(records[0].read_text(encoding="utf-8"))["status"], "failed")

    def test_corrupt_job_is_unknown_not_running(self):
        with patch.object(transcription, "launch_worker"):
            job = self.start()["data"]["jobId"]
        path = self.state / "jobs" / job / "job.json"
        path.write_text('{"status":"running","deadline":0}', encoding="utf-8")
        self.assert_error(self.query(job), "ASR_UNKNOWN")
        self.assert_error(self.start(), "ASR_UNKNOWN")

    def test_publication_uses_same_account_for_check_and_cache_identity(self):
        settings = adapter.validate_settings(self.settings)
        video = {"bvid":BV, "part":2, "cid":202}
        # Even if the login changes immediately after the check, the saved key
        # stays bound to the checked account, never the next file read.
        with patch.object(adapter, "credential_identity", side_effect=["old", "new"]):
            snapshot = adapter.save_transcript(settings, video, "local_asr",
                [{"startMs":0,"endMs":1,"text":"one"}], expected_account="old")
        value = json.loads((self.state / "transcripts" / (snapshot + ".json")).read_text(encoding="utf-8"))
        self.assertEqual(value["identity"], adapter.digest(["old",BV,2,"local_asr"]))
        with patch.object(adapter, "credential_identity", return_value="new"):
            with self.assertRaises(adapter.AdapterError) as caught:
                adapter.save_transcript(settings, video, "local_asr", [], expected_account="old")
        self.assertEqual(caught.exception.code, "CREDENTIAL_CHANGED")

    def test_python_symlink_is_preserved(self):
        alias = self.root / "python alias"
        try: alias.symlink_to(sys.executable)
        except OSError: self.skipTest("Creating executable symlinks is unavailable")
        value = adapter.validate_settings(dict(self.settings, asrPython=str(alias)))
        self.assertEqual(value["asrPython"], str(alias))

    def test_cli_query_survives_original_caller(self):
        job = self.start()["data"]["jobId"]
        self.wait(job)
        settings = self.root / "settings.json"
        settings.write_text(json.dumps(self.settings), encoding="utf-8")
        result = subprocess.run([sys.executable,"-B",str(ROOT / "plugins/bilibili/adapter.py"),
                                 "--settings",str(settings),"--operation","transcription_status"],
                                input=json.dumps({"job_id":job}), capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["data"]["status"], "completed")
        self.assertEqual(result.stderr, "")


if __name__ == "__main__":
    unittest.main()
