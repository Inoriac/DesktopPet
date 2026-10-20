#!/usr/bin/env python3
"""Preview/apply QQ bindings for one existing role. Stop the pet before apply."""
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
    from .common import AdapterError, decode_json, load_settings, package_external_path
else:
    from common import AdapterError, decode_json, load_settings, package_external_path

PACKAGE = Path(__file__).resolve().parent
PLUGIN_ID = "desktop-pet.qq"
TOOL_ID, SKILL_ID, SOURCE_ID = (PLUGIN_ID + suffix for suffix in (".tools", ".skill", ".events"))
BINDING_ID = PLUGIN_ID + ".account"


class ConfigurationError(Exception):
    pass


def json_bytes(value):
    return (json.dumps(value, ensure_ascii=False, allow_nan=False, indent=2) + "\n").encode("utf-8")


def snapshot(path):
    if path.is_symlink():
        raise ConfigurationError("Managed configuration must not be a symbolic link: " + str(path))
    try:
        return path.read_bytes() if path.exists() else None
    except OSError:
        raise ConfigurationError("Cannot read configuration: " + str(path)) from None


def read_object(raw, label):
    try:
        value = decode_json(raw) if raw is not None else {}
        if not isinstance(value, dict):
            raise ValueError()
        return value
    except (ValueError, UnicodeError):
        raise ConfigurationError(label + " must be a UTF-8 JSON object") from None


def conversation_id(profile, connection, chat):
    material = profile + json.dumps(["qq", connection, chat], ensure_ascii=False, separators=(",", ":"))
    return str(uuid.uuid5(uuid.UUID("312f2a64-cdc2-4fb7-a004-1f398bf10dca"), material))


class ConfigurationPlan:
    def __init__(self, profile, settings_path, host, originals):
        self.profile, self.settings_path, self.host, self.originals = profile, settings_path, host, originals
        self.host_path = profile / "plugins.json"

    def preview(self):
        return {"mode": "preview", "profileId": self.profile.name,
                "hostConfigurationPath": str(self.host_path), "connectionSettingsPath": str(self.settings_path),
                "hostConfiguration": self.host}


def prepare_configuration(profile_dir, settings_path, autonomous="on", source="off"):
    profile = Path(profile_dir).expanduser().resolve()
    try:
        if str(uuid.UUID(profile.name)) != profile.name or not profile.is_dir():
            raise ValueError()
        package_external_path(str(profile), "profile-dir")
        settings_path = Path(package_external_path(str(Path(settings_path).expanduser().absolute()), "settings"))
        settings_raw = snapshot(settings_path)
        settings = load_settings(str(settings_path))  # Deliberately does not read tokenFile or contact the service.
    except (ValueError, AdapterError) as error:
        raise ConfigurationError(error.message if isinstance(error, AdapterError) else
                                 "profile-dir must be an existing canonical lowercase UUID directory") from None
    if autonomous not in ("on", "off") or source not in ("on", "off"):
        raise ConfigurationError("autonomous and source must be on or off")
    interpreter = os.path.abspath(sys.executable)  # Keep venv symlinks: resolve() would select the system environment.
    if not Path(interpreter).is_file():
        raise ConfigurationError("The current Python interpreter is unavailable")
    host_path = profile / "plugins.json"
    if settings_path == host_path:
        raise ConfigurationError("settings must be separate from plugins.json")
    originals = {host_path: snapshot(host_path), settings_path: settings_raw}
    host = copy.deepcopy(read_object(originals[host_path], "plugins.json"))
    for key in ("plugins", "grants", "observationBindings", "observationAdapters", "replyBindings"):
        if not isinstance(host.get(key, []), list) or any(not isinstance(item, dict) for item in host.get(key, [])):
            raise ConfigurationError(key + " must be an array of objects")
    for key in ("configuration", "resultAdapters"):
        if not isinstance(host.get(key, {}), dict) or any(not isinstance(item, dict) for item in host.get(key, {}).values()):
            raise ConfigurationError(key + " must map names to objects")
    for key in ("replyBindings", "observationAdapters"):
        if any(not isinstance(item.get("ref", {}), dict) for item in host.get(key, [])):
            raise ConfigurationError(key + " ref must be an object")
    plugins = []
    for item in host.get("plugins", []):
        identity = item.get("pluginId")
        if not identity and isinstance(item.get("rootPath"), str):
            manifest = Path(item["rootPath"]) / "plugin.json"
            if manifest.is_file():
                identity = read_object(snapshot(manifest), "plugin.json").get("id")
        if identity != PLUGIN_ID:
            plugins.append(item)
    host["plugins"] = plugins + [{"pluginId": PLUGIN_ID, "rootPath": str(PACKAGE), "enabled": True}]
    for entry, script in ((TOOL_ID, "adapter.py"), (SOURCE_ID, "source.py")):
        host.setdefault("configuration", {}).setdefault(entry, {}).update({
            "program": interpreter, "script": str(PACKAGE / script), "settings": str(settings_path)})
    ref = lambda operation: {"pluginId": PLUGIN_ID, "entryId": TOOL_ID, "entryVersion": "1.0.0", "operationId": operation}
    identity = {"provider": "qq", "namespace": settings["connectionKey"], "remoteActorId": settings["selfId"]}
    bindings = [item for item in host.get("observationBindings", []) if item.get("pluginId") != PLUGIN_ID]
    if any(item.get("id") == BINDING_ID for item in bindings):
        raise ConfigurationError("Generated observation binding ID conflicts with another plugin")
    bindings.append({"id": BINDING_ID, "pluginId": PLUGIN_ID, "channel": "qq",
                     "connectionKey": settings["connectionKey"], "provider": "qq", "namespace": settings["connectionKey"],
                     "selfIdentity": identity,
                     "conversations": [{"remoteConversationId": chat, "kind": "group" if chat.startswith("group:") else "direct"}
                                       for chat in settings["allowedConversations"]],
                     "source": {"sourceId": SOURCE_ID, "configurationKey": SOURCE_ID, "executor": interpreter, "enabled": source == "on"}})
    host["observationBindings"] = bindings
    adapters = [item for item in host.get("observationAdapters", []) if item.get("ref", {}).get("pluginId") != PLUGIN_ID]
    adapters.append({"ref": ref("read"), "observationBindingId": BINDING_ID, "itemsPointer": "/items",
                     "fields": {"eventId": "/event_id", "sequence": "/sequence", "cursor": "/cursor",
                                "remoteConversationId": "/chat_id", "remoteMessageId": "/message_id", "text": "/text",
                                "remoteActorId": "/speaker_id", "speakerDisplayName": "/display_name",
                                "occurredAt": "/occurred_at", "replyToRemoteId": "/reply_to", "historicalImport": "/historical"}})
    host["observationAdapters"] = adapters
    grants = [item for item in host.get("grants", []) if item.get("pluginId") != PLUGIN_ID]
    replies = [item for item in host.get("replyBindings", []) if item.get("ref", {}).get("pluginId") != PLUGIN_ID]

    def grant(entry, operation=None, chat=None, internal=False):
        scope = conversation_id(profile.name, settings["connectionKey"], chat) if chat else "internal" if internal else "local_owner"
        item = {"id": PLUGIN_ID + "." + scope + "." + (operation or "skill"), "profileId": profile.name,
                "pluginId": PLUGIN_ID, "entryId": entry, "effects": ["write" if operation == "send" else "read"],
                "autonomous": autonomous == "on" if chat or internal else False}
        item.update({"scope": "internal_activity"} if internal else {"conversationId": scope})
        if operation:
            item.update({"operationId": operation, "executor": interpreter})
            if chat:
                item["objectArguments"] = {"/chat_id": chat}
        if any(existing.get("id") == item["id"] for existing in grants):
            raise ConfigurationError("Generated grant ID conflicts with another plugin")
        grants.append(item)
        return item["id"]

    for internal in ([False, True] if autonomous == "on" else [False]):
        grant(SKILL_ID, internal=internal)
        for operation in ("status", "list", "read", "receipt"):
            grant(TOOL_ID, operation, internal=internal)
    for chat in settings["allowedConversations"]:
        grant(SKILL_ID, chat=chat)
        for operation in ("read", "send", "receipt"):
            grant_id = grant(TOOL_ID, operation, chat=chat)
            if operation == "send":
                replies.append({"channel": "qq", "connectionKey": settings["connectionKey"], "remoteConversationId": chat,
                                "kind": "group" if chat.startswith("group:") else "direct", "selfIdentity": identity,
                                "ref": ref("send"), "grantRef": grant_id,
                                "argumentBindings": {"/chat_id": "remoteConversationId", "/text": "content", "/reply_to": "replyToRemoteId"}})
    host["grants"], host["replyBindings"] = grants, replies
    for operation in ("status", "list", "read", "send", "receipt"):
        mapping = {"dataPointer": "/data", "statePointer": "/state",
                   "stateMap": {"completed": "completed", "failed": "failed", "unknown": "unknown"}}
        if operation == "send":
            mapping.update({"receiptPointer": "/receipt", "trackingPointer": "/tracking"})
        host.setdefault("resultAdapters", {})[TOOL_ID + "/" + operation] = mapping
    return ConfigurationPlan(profile, settings_path, host, originals)


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
    for path, original in plan.originals.items():
        if snapshot(path) != original:
            raise ConfigurationError("Configuration changed after preview; prepare it again")
    content = json_bytes(plan.host)
    changed = content != plan.originals[plan.host_path]
    result = dict(plan.preview(), mode="applied", changed=changed, backupDirectory=None)
    if not changed:
        return result
    backup = plan.profile / "qq-backups" / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S") + "-" + uuid.uuid4().hex)
    try:
        backup.mkdir(mode=0o700, parents=True)
        original = plan.originals[plan.host_path]
        if original is not None:
            atomic_write(backup / "plugins.json", original)
        atomic_write(backup / "manifest.json", json_bytes({"profileId": plan.profile.name, "pluginsJsonExisted": original is not None}))
        atomic_write(plan.host_path, content)
    except OSError:
        raise ConfigurationError("Configuration apply failed; original plugins.json was preserved") from None
    result["backupDirectory"] = str(backup)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile-dir", required=True)
    parser.add_argument("--settings", required=True)
    parser.add_argument("--autonomous", choices=("on", "off"), default="on")
    parser.add_argument("--source", choices=("on", "off"), default="off")
    parser.add_argument("--apply", action="store_true")
    options = parser.parse_args()
    try:
        plan = prepare_configuration(options.profile_dir, options.settings, options.autonomous, options.source)
        output, code = (apply_configuration(plan) if options.apply else plan.preview()), 0
    except (ConfigurationError, OSError) as error:
        output, code = {"error": {"code": "CONFIGURATION_ERROR", "message": str(error)}}, 1
    sys.stdout.buffer.write(json_bytes(output))
    return code


if __name__ == "__main__":
    sys.exit(main())
