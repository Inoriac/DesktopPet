#!/usr/bin/env python3
"""Preview or apply this package's host configuration to one explicit profile.

Only Python's standard library and the adjacent adapter are needed. Stop the pet
before applying: individual replacements are atomic, the pair is not crash-atomic.
"""
import argparse
import copy
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import sys
import tempfile
import uuid

from adapter import AdapterError, decode_json, validate_settings


PACKAGE = Path(__file__).resolve().parent
PLUGIN_ID = "desktop-pet.lx-music"
TOOL_ID = PLUGIN_ID + ".tools"
SKILL_ID = PLUGIN_ID + ".skill"
MAX_CONFIG_BYTES = 2 * 1024 * 1024


class ConfigurationError(Exception):
    pass


def json_bytes(value):
    return (json.dumps(value, ensure_ascii=False, allow_nan=False, indent=2) + "\n").encode("utf-8")


def absolute(value):
    try:
        return Path(value).expanduser().resolve()
    except (OSError, ValueError, RuntimeError):
        raise ConfigurationError("Invalid filesystem path") from None


def snapshot(path):
    if path.is_symlink():
        raise ConfigurationError("Refusing a symbolic link for managed file: " + str(path))
    if not path.exists():
        return None
    if not path.is_file():
        raise ConfigurationError("Managed path is not a regular file: " + str(path))
    try:
        with path.open("rb") as source:
            content = source.read(MAX_CONFIG_BYTES + 1)
    except OSError:
        raise ConfigurationError("Cannot read managed file: " + str(path)) from None
    if len(content) > MAX_CONFIG_BYTES:
        raise ConfigurationError("Configuration exceeds 2 MiB")
    return content


def read_object(raw, label):
    if raw is None:
        raise ConfigurationError(label + " is missing")
    try:
        value = decode_json(raw)
    except AdapterError:
        raise ConfigurationError(label + " must be valid UTF-8 JSON without duplicate keys") from None
    if not isinstance(value, dict):
        raise ConfigurationError(label + " must be a JSON object")
    return value


def validate_host(config, profile_id):
    for key in ("plugins", "grants", "replyBindings", "observationBindings", "observationAdapters"):
        if key in config and (not isinstance(config[key], list)
                              or any(not isinstance(item, dict) for item in config[key])):
            raise ConfigurationError(key + " must be an array of objects")
    for key in ("configuration", "connections", "resultAdapters"):
        if key in config and (not isinstance(config[key], dict)
                              or any(not isinstance(item, dict) for item in config[key].values())):
            raise ConfigurationError(key + " must map names to objects")
    grant_ids = set()
    for grant in config.get("grants", []):
        for key in ("id", "profileId", "pluginId", "entryId"):
            if not isinstance(grant.get(key), str) or not grant[key]:
                raise ConfigurationError("Existing grant has an invalid " + key)
        if grant["id"] in grant_ids or grant["profileId"] != profile_id:
            raise ConfigurationError("Duplicate grant or grant belongs to another profile")
        grant_ids.add(grant["id"])
        if "scope" in grant:
            if grant["scope"] != "internal_activity" or "conversationId" in grant:
                raise ConfigurationError("Existing grant has an invalid scope")
        elif not isinstance(grant.get("conversationId"), str) or not grant["conversationId"]:
            raise ConfigurationError("Existing grant requires a conversationId")
        if "autonomous" in grant and type(grant["autonomous"]) is not bool:
            raise ConfigurationError("Existing autonomous flag must be boolean")
        if "effects" in grant and (not isinstance(grant["effects"], list)
                                    or any(effect not in ("read", "write", "conditional")
                                           for effect in grant["effects"])):
            raise ConfigurationError("Existing grant has invalid effects")
        for key in ("executor", "operationId"):
            if key in grant and not isinstance(grant[key], str):
                raise ConfigurationError("Existing grant has invalid " + key)
        if grant["pluginId"] != PLUGIN_ID and grant["entryId"] in (TOOL_ID, SKILL_ID):
            raise ConfigurationError("Music entry is granted under a conflicting plugin identity")


def merge_plugin_entries(entries):
    result = []
    seen = set()
    found = False
    for entry in entries:
        root = entry.get("rootPath")
        if not isinstance(root, str) or not root or not Path(root).is_absolute():
            raise ConfigurationError("Existing plugin rootPath must be absolute")
        if "enabled" in entry and type(entry["enabled"]) is not bool:
            raise ConfigurationError("Existing plugin enabled flag must be boolean")
        manifest_path = absolute(root) / "plugin.json"
        raw = snapshot(manifest_path)
        if raw is None:
            raise ConfigurationError("Cannot identify existing plugin: " + root)
        actual_id = read_object(raw, str(manifest_path)).get("id")
        if not isinstance(actual_id, str) or not actual_id:
            raise ConfigurationError("Existing plugin manifest is missing its identity")
        if "pluginId" in entry and entry["pluginId"] != actual_id:
            raise ConfigurationError("Existing pluginId conflicts with its manifest")
        if actual_id in seen:
            raise ConfigurationError("Duplicate plugin identity: " + actual_id)
        seen.add(actual_id)
        if actual_id == PLUGIN_ID:
            result.append(dict(entry, pluginId=PLUGIN_ID, rootPath=str(PACKAGE), enabled=True))
            found = True
        else:
            result.append(entry)
    if not found:
        result.append({"pluginId": PLUGIN_ID, "rootPath": str(PACKAGE), "enabled": True})
    return result


class ConfigurationPlan:
    def __init__(self, profile, host, settings, originals):
        self.profile = profile
        self.host_path = profile / "plugins.json"
        self.settings_path = profile / "lx-music.settings.json"
        self.host = host
        self.settings = settings
        self.originals = originals

    def preview(self):
        return {"mode": "preview", "profileId": self.profile.name,
                "hostConfigurationPath": str(self.host_path),
                "connectionSettingsPath": str(self.settings_path),
                "hostConfiguration": self.host, "connectionSettings": self.settings}


def prepare_configuration(profile_dir, database, executable=None,
                          api_url="http://127.0.0.1:23330", autonomous="off"):
    profile = absolute(profile_dir)
    try:
        canonical_id = str(uuid.UUID(profile.name))
    except ValueError:
        raise ConfigurationError("profile-dir must end in a canonical lowercase UUID") from None
    if profile.name != canonical_id or not profile.is_dir():
        raise ConfigurationError("profile-dir must be an existing canonical UUID directory")
    if autonomous not in ("off", "read", "all"):
        raise ConfigurationError("autonomous must be off, read, or all")
    interpreter = absolute(sys.executable)
    if not interpreter.is_file():
        raise ConfigurationError("The current Python interpreter is unavailable")
    try:
        settings = validate_settings({"database": str(absolute(database)),
                                      "executable": str(absolute(executable)) if executable else None,
                                      "apiUrl": api_url})
    except AdapterError as error:
        raise ConfigurationError(error.message) from None
    host_path = profile / "plugins.json"
    settings_path = profile / "lx-music.settings.json"
    originals = {path: snapshot(path) for path in (settings_path, host_path)}
    host = read_object(originals[host_path], "plugins.json") if originals[host_path] is not None else {}
    validate_host(host, profile.name)
    host = copy.deepcopy(host)
    host["plugins"] = merge_plugin_entries(host.get("plugins", []))
    tool = read_object(snapshot(PACKAGE / "tool.json"), "tool.json")
    plugin = read_object(snapshot(PACKAGE / "plugin.json"), "plugin.json")
    skill = read_object(snapshot(PACKAGE / "skill.json"), "skill.json")
    if (plugin.get("id") != PLUGIN_ID or tool.get("id") != TOOL_ID or skill.get("id") != SKILL_ID
            or not (PACKAGE / "adapter.py").is_file()):
        raise ConfigurationError("This package has conflicting or missing identities")
    operations = tool.get("definition", {}).get("operations")
    if not isinstance(operations, list) or not operations:
        raise ConfigurationError("This package has no operations")
    configuration = host.setdefault("configuration", {})
    configuration.setdefault(TOOL_ID, {}).update({"program": str(interpreter),
                                               "script": str(PACKAGE / "adapter.py"),
                                               "settings": str(settings_path)})
    existing_grants = [grant for grant in host.get("grants", []) if grant["pluginId"] != PLUGIN_ID]
    generated = []
    result_adapters = host.setdefault("resultAdapters", {})
    operation_ids = set()

    def grant(entry, operation, effect, internal=False):
        item = {"id": PLUGIN_ID + (".internal." if internal else ".local.") + (operation or "skill"),
                "profileId": profile.name, "pluginId": PLUGIN_ID, "entryId": entry,
                "effects": [effect], "autonomous": internal}
        if internal:
            item["scope"] = "internal_activity"
        else:
            item["conversationId"] = "local_owner"
        if operation:
            item.update({"operationId": operation, "executor": str(interpreter)})
        generated.append(item)

    for operation in operations:
        if not isinstance(operation, dict):
            raise ConfigurationError("Invalid operation in package")
        identifier = operation.get("id")
        effect = operation.get("effects", {}).get("kind")
        if not isinstance(identifier, str) or not identifier or identifier in operation_ids or effect not in ("read", "write"):
            raise ConfigurationError("Invalid or duplicate music operation")
        operation_ids.add(identifier)
        grant(TOOL_ID, identifier, effect)
        if autonomous == "all" or (autonomous == "read" and effect == "read"):
            grant(TOOL_ID, identifier, effect, internal=True)
        result_adapters[TOOL_ID + "/" + identifier] = {
            "dataPointer": "/data", "statePointer": "/state",
            "stateMap": {"completed": "completed", "failed": "failed", "unknown": "unknown"}}
    grant(SKILL_ID, None, "read")
    if autonomous != "off":
        grant(SKILL_ID, None, "read", internal=True)
    existing_ids = {grant["id"] for grant in existing_grants}
    if any(grant["id"] in existing_ids for grant in generated):
        raise ConfigurationError("Generated grant identity conflicts with another plugin's grant")
    host["grants"] = existing_grants + generated
    validate_host(host, profile.name)
    if len(json_bytes(host)) > MAX_CONFIG_BYTES:
        raise ConfigurationError("Generated configuration exceeds 2 MiB")
    return ConfigurationPlan(profile, host, settings, originals)


def atomic_write(path, content):
    descriptor, temporary = tempfile.mkstemp(prefix="." + path.name + ".", suffix=".tmp", dir=str(path.parent))
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
    result = plan.preview()
    result.update({"mode": "applied", "changed": bool(changed), "backupDirectory": None})
    if not changed:
        return result
    backup_root = plan.profile / "lx-music-backups"
    if backup_root.is_symlink():
        raise ConfigurationError("Refusing a symbolic link for the backup directory")
    backup = backup_root / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S") + "-" + uuid.uuid4().hex)
    applied = []
    try:
        backup_root.mkdir(mode=0o700, exist_ok=True)
        backup.mkdir(mode=0o700, exist_ok=False)
        for path, original in plan.originals.items():
            if original is not None:
                with (backup / path.name).open("xb") as output:
                    output.write(original)
                    output.flush()
                    os.fsync(output.fileno())
        (backup / "manifest.json").write_bytes(json_bytes({
            "profileId": plan.profile.name,
            "files": {path.name: {"existed": original is not None} for path, original in plan.originals.items()}}))
        for path in changed:
            atomic_write(path, files[path])
            applied.append(path)
    except OSError as error:
        recovery_failures = []
        for path in reversed(applied):
            try:
                original = plan.originals[path]
                if original is None:
                    path.unlink()
                else:
                    atomic_write(path, original)
            except OSError:
                recovery_failures.append(path.name)
        message = "Configuration apply failed: " + str(error)
        if recovery_failures:
            message += "; restore " + ", ".join(recovery_failures) + " from " + str(backup)
        else:
            message += "; original managed files preserved"
        raise ConfigurationError(message) from None
    result["backupDirectory"] = str(backup)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile-dir", required=True)
    parser.add_argument("--database", required=True)
    parser.add_argument("--executable")
    parser.add_argument("--api-url", default="http://127.0.0.1:23330")
    parser.add_argument("--autonomous", choices=("off", "read", "all"), default="off")
    parser.add_argument("--apply", action="store_true")
    args = parser.parse_args()
    try:
        plan = prepare_configuration(args.profile_dir, args.database, args.executable, args.api_url, args.autonomous)
        output = apply_configuration(plan) if args.apply else plan.preview()
        exit_code = 0
    except (ConfigurationError, OSError, TypeError, ValueError, KeyError) as error:
        output = {"error": {"code": "CONFIGURATION_ERROR", "message": str(error)}}
        exit_code = 1
    sys.stdout.buffer.write(json_bytes(output))
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
