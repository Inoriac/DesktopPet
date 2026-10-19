#!/usr/bin/env python3
"""Preview/apply web access for one existing role. Stop the pet before apply."""
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

PACKAGE = Path(__file__).resolve().parent
PLUGIN_ID = "desktop-pet.web"
TOOL_ID = PLUGIN_ID + ".tools"
SKILL_ID = PLUGIN_ID + ".skill"
OPERATIONS = ("status", "search", "read_page")


class ConfigurationError(Exception):
    pass


def json_bytes(value):
    return (json.dumps(value, ensure_ascii=False, allow_nan=False, indent=2) + "\n").encode("utf-8")


def snapshot(path):
    if path.is_symlink():
        raise ConfigurationError("Managed configuration must not be a symbolic link: " + str(path))
    return path.read_bytes() if path.exists() else None


def read_object(raw, label):
    def object_pairs(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("duplicate key")
            result[key] = value
        return result

    def invalid_constant(value):
        raise ValueError("invalid JSON constant: " + value)

    try:
        value = json.loads(raw.decode("utf-8-sig"), object_pairs_hook=object_pairs,
                           parse_constant=invalid_constant) if raw is not None else {}
        if not isinstance(value, dict):
            raise ValueError("not an object")
        return value
    except (UnicodeError, ValueError):
        raise ConfigurationError(label + " must be a UTF-8 JSON object without duplicate keys") from None


def absolute_path(value, label):
    path = Path(value).expanduser()
    if not path.is_absolute():
        raise ConfigurationError(label + " must be an absolute path")
    return path.resolve()


class ConfigurationPlan:
    def __init__(self, profile, host, original):
        self.profile = profile
        self.host_path = profile / "plugins.json"
        self.host, self.original = host, original

    def preview(self):
        return {"mode": "preview", "profileId": self.profile.name,
                "hostConfigurationPath": str(self.host_path), "hostConfiguration": self.host}


def prepare_configuration(profile_dir, program, state_dir, autonomous="on"):
    profile = Path(profile_dir).expanduser().resolve()
    try:
        if str(uuid.UUID(profile.name)) != profile.name or not profile.is_dir():
            raise ValueError()
    except ValueError:
        raise ConfigurationError("profile-dir must be an existing canonical lowercase UUID directory") from None
    if autonomous not in ("on", "off"):
        raise ConfigurationError("autonomous must be on or off")
    executable = absolute_path(program, "program")
    state = absolute_path(state_dir, "state-dir")
    if not executable.is_file() or not os.access(executable, os.X_OK):
        raise ConfigurationError("program must point to the built desktop_pet_web executable")
    if executable == PACKAGE or PACKAGE in executable.parents:
        raise ConfigurationError("Keep the built executable outside the plugin package")
    if state == PACKAGE or PACKAGE in state.parents:
        raise ConfigurationError("state-dir must be outside the plugin package")
    if state.exists() and not state.is_dir():
        raise ConfigurationError("state-dir must be a directory")
    original = snapshot(profile / "plugins.json")
    host = copy.deepcopy(read_object(original, "plugins.json"))
    for key in ("plugins", "grants"):
        if not isinstance(host.get(key, []), list) or any(not isinstance(item, dict) for item in host.get(key, [])):
            raise ConfigurationError(key + " must be an array of objects")
    for key in ("configuration", "resultAdapters"):
        if not isinstance(host.get(key, {}), dict) or any(not isinstance(item, dict) for item in host.get(key, {}).values()):
            raise ConfigurationError(key + " must map names to objects")

    entries, web_entry = [], {}
    for item in host.get("plugins", []):
        identity = item.get("pluginId")
        root = item.get("rootPath")
        same_root = isinstance(root, str) and Path(root).is_absolute() and Path(root).resolve() == PACKAGE
        if same_root and identity not in (None, "", PLUGIN_ID):
            raise ConfigurationError("The web package path is registered under a different pluginId")
        if not identity and isinstance(root, str) and Path(root).is_absolute():
            manifest = Path(root) / "plugin.json"
            if manifest.is_file():
                identity = read_object(manifest.read_bytes(), "plugin.json").get("id")
        if identity == PLUGIN_ID or same_root:
            web_entry.update(item)
        else:
            entries.append(item)
    web_entry.update({"pluginId": PLUGIN_ID, "rootPath": str(PACKAGE), "enabled": True})
    host["plugins"] = entries + [web_entry]
    host.setdefault("configuration", {}).setdefault(TOOL_ID, {}).update({
        "program": str(executable), "state_dir": str(state)})
    grants = [grant for grant in host.get("grants", []) if grant.get("pluginId") != PLUGIN_ID]
    mappings = host.setdefault("resultAdapters", {})

    def grant(entry, operation=None, internal=False):
        item = {"id": PLUGIN_ID + (".internal." if internal else ".local.") + (operation or "skill"),
                "profileId": profile.name, "pluginId": PLUGIN_ID, "entryId": entry,
                "effects": ["read"], "autonomous": internal}
        item.update({"scope": "internal_activity"} if internal else {"conversationId": "local_owner"})
        if operation:
            item.update({"operationId": operation, "executor": str(executable)})
        if any(existing.get("id") == item["id"] for existing in grants):
            raise ConfigurationError("A generated grant ID conflicts with another plugin")
        grants.append(item)

    for operation in OPERATIONS:
        mappings[TOOL_ID + "/" + operation] = {
            "dataPointer": "/data", "statePointer": "/state",
            "stateMap": {"completed": "completed", "failed": "failed"}}
        grant(TOOL_ID, operation)
        if autonomous == "on":
            grant(TOOL_ID, operation, internal=True)
    grant(SKILL_ID)
    if autonomous == "on":
        grant(SKILL_ID, internal=True)
    host["grants"] = grants
    return ConfigurationPlan(profile, host, original)


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
    if snapshot(plan.host_path) != plan.original:
        raise ConfigurationError("Configuration changed after preview; prepare it again")
    content = json_bytes(plan.host)
    result = dict(plan.preview(), mode="applied", changed=content != plan.original, backupDirectory=None)
    if not result["changed"]:
        return result
    backup_root = plan.profile / "web-backups"
    if backup_root.is_symlink():
        raise ConfigurationError("Backup directory must not be a symbolic link")
    backup = backup_root / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S") + "-" + uuid.uuid4().hex)
    try:
        backup.mkdir(mode=0o700, parents=True)
        if plan.original is not None:
            atomic_write(backup / "plugins.json", plan.original)
        atomic_write(backup / "manifest.json", json_bytes({
            "profileId": plan.profile.name, "files": {"plugins.json": {"existed": plan.original is not None}}}))
        # A single replacement is sufficient: no settings file or global configuration is written.
        if snapshot(plan.host_path) != plan.original:
            raise ConfigurationError("Configuration changed before apply; prepare it again")
        atomic_write(plan.host_path, content)
    except OSError as error:
        raise ConfigurationError("Configuration apply failed; original plugins.json was preserved: " + str(error)) from None
    result["backupDirectory"] = str(backup)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile-dir", required=True)
    parser.add_argument("--program", required=True)
    parser.add_argument("--state-dir", required=True)
    parser.add_argument("--autonomous", choices=("on", "off"), default="on")
    parser.add_argument("--apply", action="store_true")
    args = parser.parse_args()
    try:
        plan = prepare_configuration(args.profile_dir, args.program, args.state_dir, args.autonomous)
        output = apply_configuration(plan) if args.apply else plan.preview()
        code = 0
    except (ConfigurationError, OSError, ValueError, RuntimeError) as error:
        output = {"error": {"code": "CONFIGURATION_ERROR", "message": str(error)}}
        code = 1
    sys.stdout.buffer.write(json_bytes(output))
    return code


if __name__ == "__main__":
    sys.exit(main())
