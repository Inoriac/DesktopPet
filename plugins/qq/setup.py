#!/usr/bin/env python3
"""Install only the QQ plugin dependency in an explicit package-external venv."""
import sys
sys.dont_write_bytecode = True

import argparse
import json
import os
from pathlib import Path
import subprocess

if __package__:
    from .common import AdapterError, package_external_path
else:
    from common import AdapterError, package_external_path

PACKAGE = Path(__file__).resolve().parent


def prepare(state_dir):
    if sys.version_info < (3, 9):
        raise AdapterError("SETUP_ERROR", "Run setup with Python 3.9 or newer")
    state = Path(package_external_path(str(Path(state_dir).expanduser().absolute()), "state-dir"))
    state.mkdir(mode=0o700, parents=True, exist_ok=True)
    environment = state / "venv"
    python = environment / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
    commands = [[os.path.abspath(sys.executable), "-B", "-m", "venv", str(environment)],
                [str(python), "-B", "-m", "pip", "install", "--no-cache-dir", "-r", str(PACKAGE / "requirements.txt")]]
    try:
        for command in commands:
            subprocess.run(command, check=True, stdout=sys.stderr, env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"))
    except (OSError, subprocess.CalledProcessError):
        raise AdapterError("SETUP_ERROR", "Dependency installation did not finish; check the output and rerun. Existing state was retained") from None
    return {"python": str(python), "stateDir": str(state)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state-dir", required=True)
    options = parser.parse_args()
    try:
        result = prepare(options.state_dir)
    except (AdapterError, OSError) as error:
        print("Setup failed: " + str(error), file=sys.stderr)
        return 1
    sys.stdout.buffer.write((json.dumps(result, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
