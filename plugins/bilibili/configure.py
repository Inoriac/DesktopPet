#!/usr/bin/env python3
"""Preview/apply Bilibili access for one existing role. Stop the pet before apply."""
import sys
sys.dont_write_bytecode = True

import argparse
import copy
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import tempfile
import uuid

if __package__:
    from .adapter import AdapterError, decode_json, validate_settings
else:
    from adapter import AdapterError, decode_json, validate_settings

PACKAGE = Path(__file__).resolve().parent
PLUGIN_ID = "desktop-pet.bilibili"
TOOL_ID = PLUGIN_ID + ".tools"
SKILL_ID = PLUGIN_ID + ".skill"


class ConfigurationError(Exception):
    pass


def json_bytes(value):
    return (json.dumps(value, ensure_ascii=False, allow_nan=False, indent=2) + "\n").encode("utf-8")


def snapshot(path):
    if path.is_symlink():
        raise ConfigurationError("Managed configuration must not be a symbolic link: " + str(path))
    if not path.exists():
        return None
    try:
        return path.read_bytes()
    except OSError:
        raise ConfigurationError("Cannot read configuration: " + str(path)) from None


def read_object(raw, label):
    try:
        value = decode_json(raw) if raw is not None else {}
        if not isinstance(value, dict):
            raise ValueError()
        return value
    except (ValueError, AdapterError):
        raise ConfigurationError(label + " must be a UTF-8 JSON object") from None


class ConfigurationPlan:
    def __init__(self, profile, host, settings, originals):
        self.profile = profile
        self.host_path = profile / "plugins.json"
        self.settings_path = profile / "bilibili.settings.json"
        self.host, self.settings, self.originals = host, settings, originals

    def preview(self):
        return {"mode": "preview", "profileId": self.profile.name,
                "hostConfigurationPath": str(self.host_path), "connectionSettingsPath": str(self.settings_path),
                "hostConfiguration": self.host, "connectionSettings": self.settings}


def prepare_configuration(profile_dir, state_dir, autonomous="on", asr_model_path=None):
    profile = Path(profile_dir).expanduser().resolve()
    try:
        if str(uuid.UUID(profile.name)) != profile.name or not profile.is_dir():
            raise ValueError()
    except ValueError:
        raise ConfigurationError("profile-dir must be an existing canonical lowercase UUID directory") from None
    if autonomous not in ("on", "off"):
        raise ConfigurationError("autonomous must be on or off")
    interpreter = os.path.abspath(sys.executable)
    if not Path(interpreter).is_file():
        raise ConfigurationError("The current Python interpreter is unavailable")
    originals = {p: snapshot(p) for p in (profile / "bilibili.settings.json", profile / "plugins.json")}
    host = copy.deepcopy(read_object(originals[profile / "plugins.json"], "plugins.json"))
    settings = read_object(originals[profile / "bilibili.settings.json"], "bilibili.settings.json")
    state = str(Path(state_dir).expanduser().resolve())
    settings.update({"stateDir": state, "credentialPath": str(Path(state) / "credential.json"),
                     "asrEnabled": asr_model_path is not None,
                     "asrPython": interpreter if asr_model_path else None,
                     "asrModelPath": str(Path(asr_model_path).expanduser().resolve()) if asr_model_path else None})
    try:
        settings = validate_settings(settings)
    except AdapterError as error:
        raise ConfigurationError(error.message) from None
    if asr_model_path and not all((Path(settings["asrModelPath"]) / name).is_file()
                                   for name in ("model.bin", "config.json", "tokenizer.json")):
        raise ConfigurationError("ASR model is incomplete; run setup --asr-model first")
    for key in ("plugins", "grants"):
        if not isinstance(host.get(key, []), list) or any(not isinstance(item, dict) for item in host.get(key, [])):
            raise ConfigurationError(key + " must be an array of objects")
    for key in ("configuration", "resultAdapters"):
        if not isinstance(host.get(key, {}), dict) or any(not isinstance(item, dict) for item in host.get(key, {}).values()):
            raise ConfigurationError(key + " must map names to objects")
    entries = []
    for item in host.get("plugins", []):
        identity = item.get("pluginId")
        if not identity and isinstance(item.get("rootPath"), str):
            manifest = Path(item["rootPath"]) / "plugin.json"
            if manifest.is_file():
                identity = read_object(snapshot(manifest), "plugin.json").get("id")
        if identity != PLUGIN_ID:
            entries.append(item)
    entries.append({"pluginId": PLUGIN_ID, "rootPath": str(PACKAGE), "enabled": True})
    host["plugins"] = entries
    host.setdefault("configuration", {}).setdefault(TOOL_ID, {}).update({
        "program": interpreter, "script": str(PACKAGE / "adapter.py"),
        "settings": str(profile / "bilibili.settings.json")})
    grants = [g for g in host.get("grants", []) if g.get("pluginId") != PLUGIN_ID]
    mappings = host.setdefault("resultAdapters", {})
    operations = read_object((PACKAGE / "tool.json").read_bytes(), "tool.json")["definition"]["operations"]

    def grant(entry, operation=None, effect="read", internal=False):
        item = {"id": PLUGIN_ID + (".internal." if internal else ".local.") + (operation or "skill"),
                "profileId": profile.name, "pluginId": PLUGIN_ID, "entryId": entry,
                "effects": [effect], "autonomous": internal}
        item.update({"scope": "internal_activity"} if internal else {"conversationId": "local_owner"})
        if operation:
            item.update({"operationId": operation, "executor": interpreter})
        if any(g.get("id") == item["id"] for g in grants):
            raise ConfigurationError("A generated grant ID conflicts with another plugin")
        grants.append(item)

    for operation in operations:
        identifier = operation["id"]
        mapping = {"dataPointer": "/data", "statePointer": "/state",
                   "stateMap": {"completed": "completed", "failed": "failed"}}
        if identifier == "start_transcription":
            mapping.update({"receiptPointer": "/receipt", "trackingPointer": "/tracking"})
            mapping["stateMap"]["accepted"] = "accepted"
        mappings[TOOL_ID + "/" + identifier] = mapping
        if identifier == "start_transcription" and not settings["asrEnabled"]:
            continue
        grant(TOOL_ID, identifier, operation["effects"]["kind"])
        if autonomous == "on":
            grant(TOOL_ID, identifier, operation["effects"]["kind"], True)
    grant(SKILL_ID)
    if autonomous == "on":
        grant(SKILL_ID, internal=True)
    host["grants"] = grants
    return ConfigurationPlan(profile, host, settings, originals)


def atomic_write(path, content):
    descriptor, temporary = tempfile.mkstemp(prefix="." + path.name + ".", dir=str(path.parent))
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(content)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def apply_configuration(plan):
    files = {plan.settings_path: json_bytes(plan.settings), plan.host_path: json_bytes(plan.host)}
    for path, original in plan.originals.items():
        if snapshot(path) != original:
            raise ConfigurationError("Configuration changed after preview; prepare it again")
    changed = [path for path, content in files.items() if content != plan.originals[path]]
    result = dict(plan.preview(), mode="applied", changed=bool(changed), backupDirectory=None)
    if not changed:
        return result
    backup = plan.profile / "bilibili-backups" / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S") + "-" + uuid.uuid4().hex)
    applied = []
    try:
        backup.mkdir(mode=0o700, parents=True)
        for path, content in plan.originals.items():
            if content is not None:
                atomic_write(backup / path.name, content)
        atomic_write(backup / "manifest.json", json_bytes({
            "profileId": plan.profile.name, "files": {p.name: {"existed": raw is not None} for p, raw in plan.originals.items()}}))
        for path in changed:
            atomic_write(path, files[path])
            applied.append(path)
    except OSError:
        failed = []
        for path in reversed(applied):
            try:
                original = plan.originals[path]
                if original is None:
                    path.unlink()
                else:
                    atomic_write(path, original)
            except OSError:
                failed.append(path.name)
        detail = "Restore " + ", ".join(failed) + " from " + str(backup) if failed else "Original managed files were preserved"
        raise ConfigurationError("Configuration apply failed. " + detail) from None
    result["backupDirectory"] = str(backup)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile-dir", required=True)
    parser.add_argument("--state-dir", required=True)
    parser.add_argument("--autonomous", choices=("on", "off"), default="on")
    parser.add_argument("--asr-model-path")
    parser.add_argument("--apply", action="store_true")
    args = parser.parse_args()
    try:
        plan = prepare_configuration(args.profile_dir, args.state_dir, args.autonomous, args.asr_model_path)
        output = apply_configuration(plan) if args.apply else plan.preview()
        code = 0
    except (ConfigurationError, OSError) as error:
        output = {"error": {"code": "CONFIGURATION_ERROR", "message": str(error)}}
        code = 1
    sys.stdout.buffer.write(json_bytes(output))
    return code


if __name__ == "__main__":
    sys.exit(main())
