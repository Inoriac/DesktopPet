#!/usr/bin/env python3
"""Prepare the plugin environment explicitly; ASR is an opt-in download."""
import sys
sys.dont_write_bytecode = True

import argparse
import json
import os
from pathlib import Path
import subprocess

if __package__:
    from .adapter import AdapterError, validate_settings
else:
    from adapter import AdapterError, validate_settings

PACKAGE = Path(__file__).resolve().parent


class SetupError(Exception):
    pass


def prepare(state_dir, asr_model=None):
    if sys.version_info < (3, 10):
        raise SetupError("Run setup with Python 3.10 or newer")
    if asr_model not in (None, "tiny", "base", "small", "medium"):
        raise SetupError("Choose tiny, base, small, or medium for ASR")
    try:
        state = Path(validate_settings({"stateDir": str(Path(state_dir).expanduser().resolve())})["stateDir"])
    except AdapterError as error:
        raise SetupError(error.message) from None
    state.mkdir(mode=0o700, parents=True, exist_ok=True)
    environment = dict(os.environ, HF_HOME=str(state / "hf-cache"), PYTHONDONTWRITEBYTECODE="1")
    venv = state / "venv"
    python = venv / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
    # python -m venv preserves an existing environment unless --clear is used.
    commands = [[os.path.abspath(sys.executable), "-B", "-m", "venv", str(venv)]]
    pip = [str(python), "-B", "-m", "pip", "install", "--no-cache-dir", "-r"]
    commands.append(pip + [str(PACKAGE / "requirements.txt")])
    result = {"python": str(python), "stateDir": str(state)}
    if asr_model:
        model = state / "models" / asr_model
        commands.append(pip + [str(PACKAGE / "requirements-asr.txt")])
        commands.append([str(python), "-B", "-c",
                         "import sys; from faster_whisper.utils import download_model; "
                         "download_model(sys.argv[1], output_dir=sys.argv[2])", asr_model, str(model)])
        result["asrModelPath"] = str(model)
    try:
        for command in commands:
            subprocess.run(command, check=True, env=environment, stdout=sys.stderr)
    except (OSError, subprocess.CalledProcessError):
        raise SetupError("Setup did not finish. Check the dependency/network output and rerun the same setup command; existing state is retained") from None
    return result


def main():
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state-dir", required=True)
    parser.add_argument("--asr-model", choices=("tiny", "base", "small", "medium"))
    args = parser.parse_args()
    try:
        result = prepare(args.state_dir, args.asr_model)
    except (SetupError, OSError) as error:
        print("Setup failed: " + str(error), file=sys.stderr)
        return 1
    sys.stdout.buffer.write((json.dumps(result, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
