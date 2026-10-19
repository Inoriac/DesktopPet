"""Test-only backend for real configured command processes and fixed ASR workers."""
import sys
sys.dont_write_bytecode = True

import argparse
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from plugins.bilibili import adapter

BV = "BV1xx411c7mD"


class Backend:
    def __init__(self, settings):
        self.state = Path(settings["stateDir"])

    def record(self, operation):
        self.state.mkdir(parents=True, exist_ok=True)
        with (self.state / "backend-calls.jsonl").open("a", encoding="utf-8") as output:
            output.write(json.dumps({"operation": operation}) + "\n")

    def video(self, bvid, part):
        return {"bvid": bvid, "aid": 1, "cid": 202 if part == 2 else 101, "part": part,
                "partCount": 2, "title": "宇宙🙂", "partTitle": "第二段", "durationSeconds": 60,
                "description": "来自测试的有界视频材料", "author": "测试作者"}

    async def search(self, keyword, page, limit, order):
        self.record("search")
        return {"entries": [{"bvid": BV, "title": "宇宙🙂", "author": "测试作者", "duration": "01:00",
                             "description": "第二分 P 的字幕"}], "total": 1, "hasMore": False}

    async def video_info(self, bvid, part):
        self.record("video_info")
        return self.video(bvid, part)

    async def comments(self, bvid, page, limit):
        self.record("comments")
        return {"entries": [{"id": "1", "userId": "2", "author": "观众", "text": "很有趣🙂", "likes": 1, "time": 1}],
                "total": 1, "hasMore": False}

    async def transcript(self, bvid, part):
        self.record("transcript")
        return {"video": self.video(bvid, part), "language": "zh",
                "segments": [{"startMs": 1250, "endMs": 5000, "text": "字幕🙂" * 900}]}


def prepare_profile(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("--prepare-profile", required=True)
    parser.add_argument("--state-dir", required=True)
    parser.add_argument("--autonomous", choices=("on", "off"), default="on")
    parser.add_argument("--asr", action="store_true")
    args = parser.parse_args(argv)
    profile, state = Path(args.prepare_profile), Path(args.state_dir)
    profile.mkdir(parents=True, exist_ok=True)
    state.mkdir(parents=True, exist_ok=True)
    command = [sys.executable, "-B", str(ROOT / "plugins" / "bilibili" / "configure.py"),
               "--profile-dir", str(profile), "--state-dir", str(state), "--autonomous", args.autonomous, "--apply"]
    if args.asr:
        from bilibili_asr_engine import install
        model = state / "model"
        model.mkdir(exist_ok=True)
        for name in ("model.bin", "config.json", "tokenizer.json"):
            (model / name).write_text("{}", encoding="utf-8")
        install(state / "test-dependencies")
        command += ["--asr-model-path", str(model)]
    completed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if completed.returncode:
        sys.stderr.buffer.write(completed.stdout + completed.stderr)
        return completed.returncode
    path = profile / "plugins.json"
    host = json.loads(path.read_text(encoding="utf-8"))
    # Host-owned binding injection exists only in this test fixture, never as
    # production adapter arguments/settings or as an alternate worker program.
    host["configuration"]["desktop-pet.bilibili.tools"]["script"] = str(Path(__file__).resolve())
    path.write_text(json.dumps(host, ensure_ascii=False), encoding="utf-8")
    return 0


if __name__ == "__main__":
    if "--prepare-profile" in sys.argv:
        sys.exit(prepare_profile(sys.argv[1:]))
    settings = adapter.load_settings(sys.argv[sys.argv.index("--settings") + 1])
    deps = Path(settings["stateDir"]) / "test-dependencies"
    if deps.is_dir():
        os.environ["PYTHONPATH"] = str(deps)
    sys.exit(adapter.main(backend=Backend(settings)))
