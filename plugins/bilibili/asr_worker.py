#!/usr/bin/env python3
"""Fixed local ASR supervisor and engine; not a model-callable command surface."""
import sys
sys.dont_write_bytecode = True

import argparse
from http.cookiejar import Cookie
import json
from pathlib import Path
import shutil
import subprocess
import time

if __package__:
    from .adapter import AdapterError, atomic_json, credential_identity, save_transcript
    from .backend import read_credential
    from .transcription import load_job, process_options
else:
    from adapter import AdapterError, atomic_json, credential_identity, save_transcript
    from backend import read_credential
    from transcription import load_job, process_options


class QuietLogger:
    """Downloader diagnostics may include signed URLs; none enter job output."""
    def debug(self, *args, **kwargs): pass
    def warning(self, *args, **kwargs): pass
    def error(self, *args, **kwargs): pass


def download_audio(settings, video, work):
    from yt_dlp import YoutubeDL
    options = {"format": "bestaudio/best", "outtmpl": str(work / "audio.%(ext)s"),
               "quiet": True, "no_warnings": True, "logger": QuietLogger(),
               "noplaylist": True, "retries": 0, "fragment_retries": 0, "extractor_retries": 0,
               "socket_timeout": settings["requestTimeoutSeconds"], "fixup": "never",
               "hls_prefer_native": True, "overwrites": True, "cachedir": False}
    try:
        with YoutubeDL(options) as downloader:
            # Keep cookies in memory. No cookie argument, export file or signed
            # URL is placed in a command line or persisted job result.
            aliases = {"sessdata":"SESSDATA", "dedeuserid":"DedeUserID"}
            for name, value in read_credential(settings).items():
                name = aliases.get(name, name)
                if name in {"SESSDATA", "bili_jct", "buvid3", "buvid4", "DedeUserID", "ac_time_value"}:
                    downloader.cookiejar.set_cookie(Cookie(0, name, value, None, False,
                        ".bilibili.com", True, True, "/", True, True, None, True, None, None, {}))
            # An explicit p=1 also prevents anthology/playlist expansion.
            url = "https://www.bilibili.com/video/%s?p=%d" % (video["bvid"], video["part"])
            info = downloader.extract_info(url, download=False)
            expected = {video["bvid"] + "_p" + str(video["part"])}
            if video["part"] == 1:
                expected.add(video["bvid"])
            if not info or info.get("_type", "video") != "video" or info.get("id") not in expected:
                raise AdapterError("ASR_DOWNLOAD_FAILED", "The audio response did not match the requested video part")
            if info.get("duration", 0) > settings["asrMaxDurationSeconds"]:
                raise AdapterError("ASR_TOO_LONG", "The downloaded video exceeds the configured duration limit")
            downloader.process_info(info)
            audio = Path(downloader.prepare_filename(info)).resolve()
            if audio.parent != work.resolve() or not audio.is_file():
                raise AdapterError("ASR_DOWNLOAD_FAILED", "The requested audio was not downloaded")
            return audio
    except AdapterError:
        raise
    except Exception:
        raise AdapterError("ASR_DOWNLOAD_FAILED", "The requested audio could not be downloaded") from None


def transcribe_audio(settings, audio):
    try:
        from faster_whisper import WhisperModel
        model = WhisperModel(settings["asrModelPath"], device="cpu", compute_type="int8", local_files_only=True)
        segments, info = model.transcribe(str(audio), vad_filter=True)
        timed = [{"startMs": round(segment.start * 1000), "endMs": round(segment.end * 1000),
                  "text": segment.text} for segment in segments if segment.text.strip()]
        return {"segments": timed, "language": info.language}
    except Exception:
        raise AdapterError("ASR_ENGINE_FAILED", "Local speech decoding or transcription failed") from None


def execute(directory):
    record = load_job(directory / "job.json")
    work = directory / "work"
    work.mkdir(exist_ok=True)
    try:
        if credential_identity(record["settings"]) != record["account"]:
            raise AdapterError("CREDENTIAL_CHANGED", "The login changed before audio acquisition")
        audio = download_audio(record["settings"], record["video"], work)
        result = transcribe_audio(record["settings"], audio)
    except AdapterError as error:
        result = {"error": {"code": error.code, "message": error.message}}
    atomic_json(work / "result.json", result)


def supervise(directory):
    path = directory / "job.json"
    record = load_job(path)
    if record["status"] != "queued":
        return 0
    record["status"] = "running"
    atomic_json(path, record)
    work = directory / "work"
    try:
        remaining = record["deadline"] - time.time()
        if remaining <= 0:
            raise subprocess.TimeoutExpired("asr_worker", 0)
        process = subprocess.run([record["settings"]["asrPython"], "-B", str(Path(__file__).resolve()),
                                  "--execute", str(directory)],
                                 stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                 timeout=remaining, **process_options())
        if process.returncode:
            raise AdapterError("ASR_ENGINE_FAILED", "The local transcription engine stopped unexpectedly")
        result = json.loads((work / "result.json").read_text(encoding="utf-8"))
        if "error" in result:
            raise AdapterError(result["error"]["code"], result["error"]["message"])
        snapshot = save_transcript(record["settings"], record["video"], "local_asr", result["segments"],
                                   language=result["language"], expected_account=record["account"])
        has_speech = bool(result["segments"])
        record.update(status="completed", snapshotId=snapshot,
                      availability="available" if has_speech else "unavailable", reason="" if has_speech else "no_speech")
    except subprocess.TimeoutExpired:
        record.update(status="failed", error={"code":"ASR_TIMEOUT", "message":"The local transcription exceeded its execution deadline"})
    except AdapterError as error:
        record.update(status="failed", error={"code":error.code, "message":error.message})
    except Exception:
        record.update(status="failed", error={"code":"ASR_ENGINE_FAILED", "message":"The local transcription could not produce a reliable result"})
    finally:
        shutil.rmtree(work, ignore_errors=True)
    record["finished"] = time.time()
    atomic_json(path, record)
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--job", type=Path)
    mode.add_argument("--execute", type=Path)
    options = parser.parse_args(argv)
    if options.check:
        try:
            import yt_dlp
            import faster_whisper
        except Exception:
            return 1
        return 0
    if options.execute:
        execute(options.execute)
        return 0
    return supervise(options.job)


if __name__ == "__main__":
    sys.exit(main())
