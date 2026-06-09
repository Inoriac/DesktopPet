#!/usr/bin/env python3
"""GENIE / genie-tts JSON-lines worker for Desktop-Pet.

Stdout is reserved for machine-readable JSON events. Diagnostic logs go to stderr.
"""

from __future__ import annotations

import contextlib
import asyncio
import json
import os
import re
import sys
import traceback
import unicodedata
import uuid
from pathlib import Path
from typing import Any, Dict, Optional


genie = None
CONFIG: Dict[str, Any] = {}
CHARACTER_NAME = ""
CHARACTER_LANGUAGE = "chinese"
CONFIGURED = False
BASE_CWD = Path.cwd()
ROBERTA_PATCHED = False


PREDEFINED = {"feibi", "mika", "thirtyseven"}
PREDEFINED_LANGUAGE = {
    "feibi": "chinese",
    "mika": "japanese",
    "thirtyseven": "english",
}

LATIN_TOKEN_REPLACEMENTS = {
    "ai": "人工智能",
    "api": "接口",
    "desktop": "桌面",
    "desktop-pet": "桌宠",
    "desktop_pet": "桌宠",
    "gpt": "大语言模型",
    "llm": "大语言模型",
    "lx": "音乐",
    "lxmusic": "音乐",
    "milltina": "米尔蒂娜",
    "openai": "欧盆人工智能",
    "qt": "扣特",
    "tts": "语音合成",
    "url": "链接",
    "wttr": "天气服务",
}

CHINESE_DIGITS = "零一二三四五六七八九"
CHINESE_UNITS = ["", "十", "百", "千"]


def emit(event: Dict[str, Any]) -> None:
    print(json.dumps(event, ensure_ascii=False), flush=True)


def log(message: str) -> None:
    print(message, file=sys.stderr, flush=True)


def normalize_voice_language(language: str) -> str:
    value = str(language or "").strip().lower()
    if value in {"zh", "cn", "chinese", "中文"}:
        return "chinese"
    if value in {"ja", "jp", "japanese", "日语"}:
        return "japanese"
    if value in {"en", "english", "英语"}:
        return "english"
    if value in {"ko", "kr", "korean", "韩语"}:
        return "korean"
    return value or "chinese"


def as_path(value: str) -> Path:
    path = Path(value).expanduser()
    if path.is_absolute():
        return path.resolve()
    return (BASE_CWD / path).resolve()


def voice_runtime_root(config: Dict[str, Any]) -> Optional[Path]:
    character_models_dir = str(config.get("characterModelsDir", "")).strip()
    if character_models_dir:
        models_root = as_path(character_models_dir)
        return models_root.parent if models_root.name.lower() == "charactermodels" else models_root

    genie_data_dir = str(config.get("genieDataDir", "")).strip()
    if genie_data_dir:
        data_root = as_path(genie_data_dir)
        return data_root.parent if data_root.name.lower() == "geniedata" else data_root

    return None


def set_env_before_import(config: Dict[str, Any]) -> None:
    genie_data_dir = str(config.get("genieDataDir", "")).strip()
    if genie_data_dir:
        os.environ["GENIE_DATA_DIR"] = str(as_path(genie_data_dir))

    runtime_root = voice_runtime_root(config)
    if runtime_root:
        runtime_root.mkdir(parents=True, exist_ok=True)
        os.chdir(runtime_root)


def import_genie() -> Any:
    global genie
    if genie is not None:
        return genie
    try:
        with contextlib.redirect_stdout(sys.stderr):
            import genie_tts as imported_genie  # type: ignore
        genie = imported_genie
        return genie
    except ModuleNotFoundError as exc:
        raise RuntimeError("genie-tts 未安装，请先运行 tools/voice/setup_voice_env.ps1") from exc


def patch_genie_runtime_for_desktop(api: Any) -> None:
    """Make the bundled GENIE runtime more tolerant for short Chinese assistant replies.

    The optional Chinese RoBERTa path can produce phone/BERT length mismatches for
    punctuation-heavy desktop assistant text. GENIE documents RoBERTa as optional,
    so the worker disables it and lets GENIE use zero BERT features instead.
    """
    del api
    global ROBERTA_PATCHED
    if ROBERTA_PATCHED:
        return

    try:
        from genie_tts.ModelManager import model_manager  # type: ignore

        def disabled_roberta_loader(*_args: Any, **_kwargs: Any) -> bool:
            return False

        model_manager.roberta_model = None
        model_manager.roberta_tokenizer = None
        model_manager.load_roberta_model = disabled_roberta_loader  # type: ignore[method-assign]
        ROBERTA_PATCHED = True
        log("GENIE Chinese RoBERTa features disabled for stable desktop synthesis.")
    except Exception as exc:  # noqa: BLE001 - best-effort runtime patch.
        log(f"GENIE runtime patch skipped: {exc}")


def int_to_chinese(value: int) -> str:
    if value == 0:
        return CHINESE_DIGITS[0]
    if value < 0:
        return "负" + int_to_chinese(abs(value))
    if value >= 10000:
        high = value // 10000
        low = value % 10000
        result = int_to_chinese(high) + "万"
        if low:
            if low < 1000:
                result += "零"
            result += int_to_chinese(low)
        return result

    parts = []
    zero_pending = False
    unit_index = 0
    current = value
    while current > 0:
        digit = current % 10
        if digit == 0:
            if parts:
                zero_pending = True
        else:
            segment = CHINESE_DIGITS[digit] + CHINESE_UNITS[unit_index]
            if zero_pending:
                parts.append(CHINESE_DIGITS[0])
                zero_pending = False
            parts.append(segment)
        current //= 10
        unit_index += 1

    spoken = "".join(reversed(parts)).rstrip(CHINESE_DIGITS[0])
    if spoken.startswith("一十"):
        spoken = spoken[1:]
    return spoken or CHINESE_DIGITS[0]


def number_to_chinese(value: str) -> str:
    text = str(value or "").strip()
    if not text:
        return ""
    sign = ""
    if text.startswith("-"):
        sign = "负"
        text = text[1:]
    elif text.startswith("+"):
        text = text[1:]

    if "." in text:
        integer, fraction = text.split(".", 1)
        integer_text = int_to_chinese(int(integer or "0"))
        fraction_text = "".join(CHINESE_DIGITS[int(ch)] for ch in fraction if ch.isdigit())
        return sign + integer_text + ("点" + fraction_text if fraction_text else "")

    return sign + int_to_chinese(int(text or "0"))


def replace_time(match: re.Match[str]) -> str:
    hour = int(match.group(1))
    minute = int(match.group(2))
    marker = (match.group(3) or "").strip().lower()
    prefix = ""
    if marker == "am":
        prefix = "上午"
    elif marker == "pm":
        prefix = "下午"
        if hour > 12:
            hour -= 12
    return f"{prefix}{int_to_chinese(hour)}点{int_to_chinese(minute)}分"


def latin_token_to_spoken(match: re.Match[str]) -> str:
    token = match.group(0)
    key = token.strip("._-+").lower()
    if key in LATIN_TOKEN_REPLACEMENTS:
        return LATIN_TOKEN_REPLACEMENTS[key]
    compact_key = re.sub(r"[^a-z0-9]", "", key)
    if compact_key in LATIN_TOKEN_REPLACEMENTS:
        return LATIN_TOKEN_REPLACEMENTS[compact_key]
    return "，"


def sanitize_chinese_tts_text(text: str) -> str:
    text = unicodedata.normalize("NFKC", text)
    text = text.replace("°C", "℃").replace("°c", "℃")
    text = text.replace("公里/小时", "公里每小时").replace("千米/小时", "千米每小时")
    text = re.sub(r"https?://\S+|www\.\S+", "链接", text, flags=re.IGNORECASE)
    text = re.sub(r"\b(\d{1,2}):(\d{2})\s*(AM|PM)?\b", replace_time, text, flags=re.IGNORECASE)
    text = re.sub(r"(-?\d+(?:\.\d+)?)\s*℃", lambda m: number_to_chinese(m.group(1)) + "摄氏度", text)
    text = re.sub(r"(-?\d+(?:\.\d+)?)\s*%", lambda m: "百分之" + number_to_chinese(m.group(1)), text)
    text = re.sub(r"(-?\d+(?:\.\d+)?)\s*(?:km/h|KM/H|公里每小时|千米每小时)",
                  lambda m: number_to_chinese(m.group(1)) + "公里每小时", text)
    text = re.sub(r"\bAM\b", "上午", text, flags=re.IGNORECASE)
    text = re.sub(r"\bPM\b", "下午", text, flags=re.IGNORECASE)
    text = re.sub(r"[A-Za-z][A-Za-z0-9_.+\-]*", latin_token_to_spoken, text)
    text = re.sub(r"-?\d+(?:\.\d+)?", lambda m: number_to_chinese(m.group(0)), text)

    replacements = {
        "&": "和",
        "/": "，",
        "\\": "，",
        "|": "，",
        "@": "，",
        "#": "号",
        "*": "，",
        "=": "等于",
        "<": "小于",
        ">": "大于",
        "~": "，",
        "`": "",
        "^": "",
    }
    for old, new in replacements.items():
        text = text.replace(old, new)

    text = text.replace("?", "？").replace("!", "！").replace(",", "，").replace(";", "；")
    text = text.replace(":", "：")
    text = re.sub(r"[^\u4e00-\u9fff\u3000-\u303f，。！？、；：‘’“”（）《》\s]", "，", text)
    text = re.sub(r"[\s\t\r\n]+", "，", text)
    text = re.sub(r"[，、；：]{2,}", "，", text)
    text = re.sub(r"，+[。！？]", lambda m: m.group(0)[-1], text)
    text = re.sub(r"[。！？]{2,}", lambda m: m.group(0)[0], text)
    return text.strip("，、；：。 ")


def sanitize_tts_text(text: str, language: str) -> str:
    clean = str(text or "").strip()
    if not clean:
        return ""
    if normalize_voice_language(language) == "chinese":
        return sanitize_chinese_tts_text(clean)
    return unicodedata.normalize("NFKC", clean)


async def run_tts_async(api: Any, text: str, save_path: str) -> tuple[int, int]:
    chunk_count = 0
    total_bytes = 0
    kwargs = {
        "character_name": CHARACTER_NAME,
        "text": text,
        "play": True,
        "split_sentence": True,
    }
    if save_path:
        kwargs["save_path"] = save_path

    async for chunk in api.tts_async(**kwargs):
        if chunk:
            chunk_count += 1
            total_bytes += len(chunk)

    api.wait_for_playback_done()
    return chunk_count, total_bytes


def expected_predefined_dir(config: Dict[str, Any], speaker: str) -> Optional[Path]:
    base = str(config.get("characterModelsDir", "")).strip()
    if not base:
        return None
    return as_path(base) / "v2ProPlus" / speaker / "tts_models"


def expected_genie_data_dir(config: Dict[str, Any]) -> Optional[Path]:
    base = str(config.get("genieDataDir", "")).strip()
    return as_path(base) if base else None


def verify_assets_if_needed(config: Dict[str, Any], speaker: str) -> None:
    if bool(config.get("allowAutoDownload", False)):
        return

    data_dir = expected_genie_data_dir(config)
    if data_dir and not data_dir.exists():
        raise FileNotFoundError(
            f"GENIE_DATA_DIR 不存在: {data_dir}. 如需自动下载，请设置 allowAutoDownload=true 或运行下载脚本。"
        )
    if data_dir:
        required = [data_dir / "chinese-hubert-base", data_dir / "speaker_encoder.onnx"]
        missing = [str(path) for path in required if not path.exists()]
        if missing:
            raise FileNotFoundError(
                "GENIE 基础资源不完整，请运行 download_genie_assets.py。缺失: " + ", ".join(missing)
            )

    if str(config.get("speakerMode", "predefined")).lower() == "predefined":
        model_dir = expected_predefined_dir(config, speaker)
        if model_dir and not model_dir.exists():
            raise FileNotFoundError(
                f"预设角色模型不存在: {model_dir}. 请运行 download_genie_assets.py --preset {speaker}。"
            )


def configure(config: Dict[str, Any], request_id: str = "") -> None:
    global CONFIG, CHARACTER_NAME, CHARACTER_LANGUAGE, CONFIGURED
    CONFIG = config
    CONFIGURED = False

    speaker_mode = str(config.get("speakerMode", "predefined")).strip().lower() or "predefined"
    selected_speaker = str(config.get("selectedSpeaker", "feibi")).strip().lower() or "feibi"
    if selected_speaker not in PREDEFINED:
        selected_speaker = "feibi"

    set_env_before_import(config)
    verify_assets_if_needed(config, selected_speaker)
    api = import_genie()
    patch_genie_runtime_for_desktop(api)

    if speaker_mode == "custom":
        custom = config.get("customSpeaker", {}) or {}
        name = str(custom.get("name", "")).strip() or "custom_voice"
        language = str(custom.get("language", "zh")).strip().lower() or "zh"
        model_dir = str(custom.get("onnxModelDir", "")).strip()
        if not model_dir:
            custom_root = str(config.get("customCharactersDir", "runtime/voice/custom_characters")).strip()
            model_dir = str(as_path(custom_root) / name / "tts_models")
        if not Path(model_dir).exists():
            raise FileNotFoundError(f"自定义角色模型目录不存在: {model_dir}")

        with contextlib.redirect_stdout(sys.stderr):
            api.load_character(character_name=name, onnx_model_dir=model_dir, language=language)
            reference_audio = str(custom.get("referenceAudioPath", "")).strip()
            reference_text = str(custom.get("referenceAudioText", "")).strip()
            if reference_audio and reference_text:
                api.set_reference_audio(
                    character_name=name,
                    audio_path=reference_audio,
                    audio_text=reference_text,
                )
        CHARACTER_NAME = name
        CHARACTER_LANGUAGE = normalize_voice_language(language)
        CONFIGURED = True
        emit({"type": "configured", "requestId": request_id, "mode": "custom", "speaker": name})
        return

    with contextlib.redirect_stdout(sys.stderr):
        api.load_predefined_character(selected_speaker)
    CHARACTER_NAME = selected_speaker
    CHARACTER_LANGUAGE = PREDEFINED_LANGUAGE.get(selected_speaker, "chinese")
    CONFIGURED = True
    emit({"type": "configured", "requestId": request_id, "mode": "predefined", "speaker": selected_speaker})


def speak(text: str, source: str, request_id: str) -> None:
    if not CONFIGURED:
        configure(CONFIG, request_id)

    raw_text = str(text or "").strip()
    clean_text = sanitize_tts_text(raw_text, CHARACTER_LANGUAGE)
    if not clean_text:
        emit({"type": "speech_skipped", "requestId": request_id, "reason": "empty_text"})
        return

    max_chars = int(CONFIG.get("maxTextChars", 350) or 350)
    if max_chars > 0 and len(clean_text) > max_chars:
        clean_text = clean_text[:max_chars]

    save_path = ""
    if bool(CONFIG.get("saveAudio", False)):
        output_dir = as_path(str(CONFIG.get("outputDir", "runtime/voice/outputs")))
        output_dir.mkdir(parents=True, exist_ok=True)
        save_path = str(output_dir / f"desktop_pet_voice_{request_id or uuid.uuid4().hex}.wav")

    api = import_genie()
    emit({"type": "speech_started", "requestId": request_id, "source": source})
    if clean_text != raw_text:
        log(f"TTS sanitized: raw_chars={len(raw_text)}, clean_chars={len(clean_text)}, text={clean_text}")
    log(f"TTS start: speaker={CHARACTER_NAME}, source={source}, chars={len(clean_text)}")

    with contextlib.redirect_stdout(sys.stderr):
        chunk_count, total_bytes = asyncio.run(run_tts_async(api, clean_text, save_path))
    if chunk_count <= 0 or total_bytes <= 0:
        raise RuntimeError("GENIE 没有生成有效音频，可能是文本或角色模型不兼容。")
    log(f"TTS finished: speaker={CHARACTER_NAME}, chunks={chunk_count}, bytes={total_bytes}, save_path={save_path or '-'}")
    emit({"type": "speech_finished", "requestId": request_id, "savePath": save_path, "chunks": chunk_count, "audioBytes": total_bytes})


def handle_message(message: Dict[str, Any]) -> bool:
    msg_type = str(message.get("type", ""))
    request_id = str(message.get("requestId", ""))

    try:
        if msg_type == "configure":
            configure(message.get("config", {}) or {}, request_id)
        elif msg_type == "speak":
            speak(str(message.get("text", "")), str(message.get("source", "assistant")), request_id)
        elif msg_type == "shutdown":
            emit({"type": "shutdown"})
            return False
        else:
            emit({"type": "error", "requestId": request_id, "code": "unknown_message", "message": msg_type})
    except Exception as exc:  # noqa: BLE001 - worker should report all failures as JSON.
        log(traceback.format_exc())
        emit({"type": "error", "requestId": request_id, "code": exc.__class__.__name__, "message": str(exc)})
    return True


def main() -> int:
    emit({"type": "ready"})
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            message = json.loads(line)
        except json.JSONDecodeError as exc:
            emit({"type": "error", "code": "invalid_json", "message": str(exc)})
            continue
        if not handle_message(message):
            break
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
