"""Persistent, optional local ASR jobs. No scheduler or autonomous retries."""
import asyncio
from contextlib import contextmanager
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time
import uuid

sys.dont_write_bytecode = True
if __package__:
    from .adapter import (AdapterError, BilibiliBackend, atomic_json, credential_identity,
                          digest, envelope, read_transcript_page)
else:
    from adapter import (AdapterError, BilibiliBackend, atomic_json, credential_identity,
                         digest, envelope, read_transcript_page)

WORKER = Path(__file__).resolve().with_name("asr_worker.py")
PENDING = frozenset({"queued", "running"})


def check_runtime(settings, timeout):
    """Import the actual configured environment without loading/downloading a model."""
    if not settings["asrEnabled"]:
        raise AdapterError("ASR_DISABLED", "Local transcription is not enabled")
    python, model = settings["asrPython"], settings["asrModelPath"]
    if not python or not Path(python).is_file() or not model or not all(
            (Path(model) / name).is_file() for name in ("model.bin", "config.json", "tokenizer.json")):
        raise AdapterError("ASR_NOT_READY", "Prepare the ASR interpreter and complete local model with the setup helper")
    try:
        result = subprocess.run([python, "-B", str(WORKER), "--check"],
                                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL, timeout=min(5, timeout),
                                **process_options())
    except (OSError, subprocess.TimeoutExpired):
        raise AdapterError("ASR_NOT_READY", "The configured ASR interpreter could not be checked in time") from None
    if result.returncode:
        raise AdapterError("DEPENDENCY_MISSING", "Prepare the ASR Python dependencies with the setup helper")


def process_options(detach=False):
    result = {"close_fds": True}
    if os.name == "nt":
        result["creationflags"] = subprocess.CREATE_NO_WINDOW
        if detach:
            result["creationflags"] |= subprocess.CREATE_NEW_PROCESS_GROUP
    elif detach:
        result["start_new_session"] = True
    return result


def launch_worker(settings, directory):
    child = subprocess.Popen([settings["asrPython"], "-B", str(WORKER), "--job", str(directory)],
                             stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, **process_options(detach=True))
    # The CLI normally exits immediately. Import-level callers also reap the
    # supervisor without waiting in this request or accumulating zombie PIDs.
    threading.Thread(target=child.wait, daemon=True).start()


def load_job(path):
    try:
        value = json.loads(Path(path).read_text(encoding="utf-8"))
        if (value["status"] not in PENDING | {"completed", "failed"}
                or not isinstance(value["deadline"], (float, int))
                or not {"jobId", "account", "video", "settings"}.issubset(value)
                or not {"bvid", "part", "cid"}.issubset(value["video"])):
            raise ValueError("Invalid record")
        if value["status"] == "completed" and not {"snapshotId", "availability"}.issubset(value):
            raise ValueError("Incomplete result")
        if value["status"] == "failed" and not {"code", "message"}.issubset(value["error"]):
            raise ValueError("Incomplete failure")
        return value
    except (OSError, ValueError, KeyError, TypeError):
        raise AdapterError("ASR_UNKNOWN", "The transcription record is unavailable; no automatic restart was attempted") from None


def job_status(record):
    # A missing supervisor cannot leave a record claiming to run forever.
    if record["status"] in PENDING and time.time() > record["deadline"] + 5:
        return "unknown"
    return record["status"]


def result_for(record):
    status = job_status(record)
    video = record["video"]
    data = {"jobId": record["jobId"], "status": status, "bvid": video["bvid"],
            "part": video["part"], "cid": video["cid"], "source": "local_asr"}
    if status == "unknown":
        data["error"] = {"code": "ASR_UNKNOWN", "message": "The worker has no reliable terminal result; no automatic restart was attempted"}
    elif status == "failed":
        data["error"] = record["error"]
    elif status == "completed":
        data.update(snapshotId=record["snapshotId"], availability=record["availability"],
                    reason=record.get("reason", ""))
    return data


@contextmanager
def submission_lock(directory, deadline):
    """Only covers the short atomic publication, never network or inference."""
    try:
        directory.parent.mkdir(parents=True, exist_ok=True)
        while True:
            try:
                directory.mkdir()
                break
            except FileExistsError:
                # An interrupted CLI may leave this short-lived lock behind.
                try:
                    if time.time() - directory.stat().st_mtime > 60:
                        directory.rmdir()
                        continue
                except FileNotFoundError:
                    continue
                if time.monotonic() >= deadline:
                    raise AdapterError("ASR_BUSY", "Another transcription request is being accepted; try again later")
                time.sleep(0.03)
        try:
            yield
        finally:
            directory.rmdir()
    except OSError:
        raise AdapterError("STATE_UNAVAILABLE", "The transcription state cannot be written") from None


def accepted(record):
    result = envelope("accepted", result_for(record))
    result["receipt"] = {"jobId": record["jobId"]}
    result["tracking"] = {"operation": "transcription_status", "arguments": {"job_id": record["jobId"]}}
    return result


def start_transcription(arguments, settings, backend=None):
    deadline = time.monotonic() + settings["requestTimeoutSeconds"]
    check_runtime(settings, settings["requestTimeoutSeconds"])
    account = credential_identity(settings)
    selected = backend if backend is not None else BilibiliBackend(settings)
    async def inspect_video():
        return await asyncio.wait_for(selected.video_info(arguments["bvid"], arguments["part"]),
                                      max(0.01, deadline - time.monotonic()))
    video = asyncio.run(inspect_video())
    if video["durationSeconds"] > settings["asrMaxDurationSeconds"]:
        raise AdapterError("ASR_TOO_LONG", "This video part exceeds the configured transcription duration limit")
    if credential_identity(settings) != account:
        raise AdapterError("CREDENTIAL_CHANGED", "The login changed while the transcription request was being checked")
    model = Path(settings["asrModelPath"])
    model_identity = [str(model)] + [(name, (model / name).stat().st_size, (model / name).stat().st_mtime_ns)
                                   for name in ("model.bin", "config.json", "tokenizer.json")]
    identity = digest([account, video["bvid"], video["part"], video["cid"], model_identity])
    jobs = Path(settings["stateDir"]) / "jobs"
    index = jobs / ("index-" + identity + ".json")
    with submission_lock(jobs / ("lock-" + identity), min(deadline, time.monotonic() + 2)):
        if index.exists():
            try:
                job_id = json.loads(index.read_text(encoding="utf-8"))["jobId"]
                if not isinstance(job_id, str) or len(job_id) != 32 or any(c not in "0123456789abcdef" for c in job_id):
                    raise ValueError("Invalid job")
            except (OSError, ValueError, KeyError):
                raise AdapterError("ASR_UNKNOWN", "The transcription index is unavailable; no automatic restart was attempted") from None
            previous = load_job(jobs / job_id / "job.json")
            state = job_status(previous)
            if state in PENDING:
                return accepted(previous)
            if state == "unknown":
                raise AdapterError("ASR_UNKNOWN", "The previous worker has no reliable terminal result; inspect its status before restarting")
            if state == "completed":
                try:
                    cached = read_transcript_page(settings, video["bvid"], video["part"], "local_asr")
                except AdapterError:
                    cached = None
                if cached and cached["snapshotId"] == previous.get("snapshotId"):
                    return envelope("completed", result_for(previous))
            # A new explicit call after a known failure may retry. There is no
            # automatic retry by the worker, status query or activity system.
        job_id = uuid.uuid4().hex
        directory = jobs / job_id
        created = time.time()
        record = {"jobId": job_id, "status": "queued", "created": created,
                  "deadline": created + settings["asrJobTimeoutSeconds"], "account": account,
                  "video": {key: video[key] for key in ("bvid", "part", "cid", "title", "durationSeconds")},
                  "settings": settings}
        atomic_json(directory / "job.json", record)
        atomic_json(index, {"jobId": job_id})
        try:
            launch_worker(settings, directory)
        except OSError:
            record.update(status="failed", error={"code":"ASR_START_FAILED", "message":"The local transcription worker could not be started"})
            atomic_json(directory / "job.json", record)
            raise AdapterError("ASR_START_FAILED", "The local transcription worker could not be started") from None
        return accepted(record)


def transcription_status(arguments, settings):
    path = Path(settings["stateDir"]) / "jobs" / arguments["job_id"] / "job.json"
    if not path.is_file():
        raise AdapterError("JOB_NOT_FOUND", "No transcription task matches this receipt under the current account")
    record = load_job(path)
    if record["account"] != credential_identity(settings):
        raise AdapterError("JOB_NOT_FOUND", "No transcription task matches this receipt under the current account")
    return envelope("completed", result_for(record))
