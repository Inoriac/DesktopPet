#!/usr/bin/env python3
"""Prepare GENIE data and predefined speakers for Desktop-Pet."""

from __future__ import annotations

import argparse
import contextlib
import os
import sys
import shutil
from pathlib import Path
from typing import Union

from huggingface_hub import snapshot_download

PRESETS = {"feibi", "mika", "thirtyseven"}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Download/prepare GENIE voice assets.")
    parser.add_argument("--genie-data-dir", default="runtime/voice/GenieData", help="GENIE_DATA_DIR path")
    parser.add_argument("--preset", action="append", choices=sorted(PRESETS), default=[], help="Predefined speaker to prepare")
    parser.add_argument("--with-roberta", action="store_true", help="Download optional Chinese RoBERTa data")
    parser.add_argument("--all", action="store_true", help="Prepare all predefined speakers")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    data_dir = Path(args.genie_data_dir).expanduser().resolve()
    runtime_root = data_dir.parent if data_dir.name.lower() == "geniedata" else data_dir.parent
    runtime_root.mkdir(parents=True, exist_ok=True)
    os.environ["GENIE_DATA_DIR"] = str(data_dir)
    os.chdir(runtime_root)

    print(f"GENIE_DATA_DIR = {data_dir}")
    print("首次基础资源下载约 391MB，请保持网络连接。")

    def download_patterns(patterns: Union[str, list[str]], label: str) -> None:
        print(f"下载 {label}...")
        snapshot_download(
            repo_id="High-Logic/Genie",
            repo_type="model",
            allow_patterns=patterns,
            local_dir=str(runtime_root),
            local_dir_use_symlinks=False,
        )

    download_patterns("GenieData/*", "GENIE 基础资源")
    if args.with_roberta:
        download_patterns("GenieData(Optional)/RoBERTa/*", "RoBERTa 资源")
        optional_roberta = runtime_root / "GenieData(Optional)" / "RoBERTa"
        roberta_dir = data_dir / "RoBERTa"
        if optional_roberta.exists():
            if roberta_dir.exists():
                shutil.rmtree(roberta_dir)
            shutil.copytree(optional_roberta, roberta_dir)

    presets = sorted(PRESETS) if args.all else args.preset
    if not presets:
        presets = ["feibi"]
    for preset in presets:
        download_patterns(f"CharacterModels/v2ProPlus/{preset}/*", f"预设角色 {preset}")

    try:
        with contextlib.redirect_stdout(sys.stderr):
            import genie_tts as genie  # type: ignore
    except ModuleNotFoundError:
        print("genie-tts 未安装。请先运行 tools/voice/setup_voice_env.ps1。", file=sys.stderr)
        return 1

    with contextlib.redirect_stdout(sys.stderr):
        for preset in presets:
            print(f"验证预设角色: {preset}")
            genie.load_predefined_character(preset)

    print("GENIE 资源准备完成。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
