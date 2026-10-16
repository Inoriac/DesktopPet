"""Bounded contract verification, not a host admission checker or tool loader.

Requires jsonschema==4.23.0 and Git. Uses temporary fixtures only; no MCP,
network, desktop actions, or real user databases. See README.md for scope.
"""

import copy
import json
import os
from pathlib import Path
import re
import runpy
import shutil
import sqlite3
import subprocess
import sys
import tempfile

from jsonschema import Draft202012Validator


ROOT = Path(__file__).resolve().parent


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate JSON key: {key}")
        result[key] = value
    return result


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique_object)


def reference_issues(package, directory):
    """Only inspect the simple, top-level property mappings in these fixtures.

    This deliberately does not resolve arbitrary business Schema references,
    compositions, dynamic definitions, or references across capability packages.
    """
    definition = package["definition"]
    issues = []
    for collection, key in [("operations", "id"), ("configuration", "key"),
                            ("requirements", "id")]:
        ids = [item[key] for item in definition[collection]]
        if len(ids) != len(set(ids)):
            issues.append(f"duplicate {collection}")
    instructions = Path(definition["instructions"])
    if instructions.is_absolute() or not (directory / instructions).is_file():
        issues.append("missing or non-relative instructions")
    settings = {item["key"]: item for item in definition["configuration"]}
    for operation in definition["operations"]:
        invocation = operation["invocation"]
        bindings = []
        for field, kind in [("program", "executable"), ("workingDirectory", "path"),
                            ("connection", "mcp_connection")]:
            if field in invocation:
                bindings.append((invocation[field]["setting"], {kind}))
        for arg in invocation.get("arguments", []):
            if not isinstance(arg, dict):
                continue
            if "setting" in arg:
                bindings.append((arg["setting"], {"executable", "path", "text", "secret"}))
            else:
                pointer = arg.get("input", arg.get("expand"))
                if pointer.count("/") != 1:
                    raise ValueError("This fixture check only supports top-level input mappings")
                key = pointer[1:].replace("~1", "/").replace("~0", "~")
                prop = operation["parameters"]["schema"].get("properties", {}).get(key, {})
                expected = "array" if "expand" in arg else "string"
                if prop.get("type") != expected:
                    issues.append("input mapping type or reference mismatch")
                elif expected == "array" and prop.get("items", {}).get("type") != "string":
                    issues.append("expanded input must contain strings")
        for key, kinds in bindings:
            setting = settings.get(key, {})
            if setting.get("kind") not in kinds or setting.get("required") is not True:
                issues.append(f"missing, optional, or incompatible setting: {key}")
    return issues


def main():
    checks = []

    def check(name, condition):
        checks.append({"name": name, "passed": bool(condition)})

    schema = read_json(ROOT / "capability.schema.json")
    Draft202012Validator.check_schema(schema)
    validator = Draft202012Validator(schema)
    examples = {kind: read_json(ROOT / "examples" / kind / "capability.json")
                for kind in ("script", "cli", "mcp")}
    for kind, package in examples.items():
        check(f"schema_{kind}", validator.is_valid(package))
        check(f"references_{kind}", not reference_issues(package, ROOT / "examples" / kind))

    # Mutation paths start at the first operation. Every negative case must
    # actually fail Schema validation; an execution exception is not a pass.
    missing = object()
    variants = [
        ("reported_description", "script", ["result", "completion"], "reported", False),
        ("accepted", "script", ["result", "completion"], "accepted", True),
        ("literal_empty_arg", "script", ["invocation", "arguments"], [""], True),
        ("mcp_inline", "mcp", ["parameters"], {"source": "inline", "schema": {"type": "object"}}, True),
        ("missing_parameters", "script", ["parameters"], missing, False),
        ("command_mcp_source", "script", ["parameters"], {"source": "mcp"}, False),
        ("mcp_json_format", "mcp", ["result", "format"], "json", False),
        ("command_mcp_format", "script", ["result", "format"], "mcp", False),
        ("ambiguous_arg", "script", ["invocation", "arguments"], [{"input": "/text", "expand": "/text"}], False),
        ("missing_scope", "cli", ["invocation", "commandScope"], missing, False),
        ("bad_business_type", "script", ["parameters", "schema", "type"], "invalid", False),
        ("non_object_input", "script", ["parameters", "schema", "type"], "array", False),
        ("invalid_id", "script", ["id"], "Bad ID", False),
        ("id_final_newline", "script", ["id"], "count\n", False),
        ("bad_pointer", "script", ["invocation", "arguments"], [{"input": "/bad~2"}], False),
        ("missing_remote_tool", "mcp", ["invocation", "tool"], missing, False),
        ("bad_output_schema", "script", ["result", "schema"], 7, False),
        ("undeclared_native", "script", ["invocation", "kind"], "native", False),
        ("undeclared_dynamic", "script", ["parameters", "source"], "dynamic", False),
        ("output_array", "script", ["result", "schema"], {"type": "array", "items": {"type": "object"}}, True),
    ]
    for name, kind, path, value, expected in variants:
        package = copy.deepcopy(examples[kind])
        target = package["definition"]["operations"][0]
        for key in path[:-1]:
            target = target[key]
        if value is missing:
            del target[path[-1]]
        else:
            target[path[-1]] = value
        check(name, validator.is_valid(package) == expected)

    for field, value in [("name", ""), ("summary", " \n"), ("formatVersion", 1),
                         ("id", "example.text\n")]:
        package = copy.deepcopy(examples["script"])
        package[field] = value
        check(f"reject_top_{field}", not validator.is_valid(package))
    package = copy.deepcopy(examples["script"])
    package["definition"]["operations"] = []
    check("reject_no_operations", not validator.is_valid(package))
    package = copy.deepcopy(examples["script"])
    package["definition"]["operations"][0]["result"].update(
        completion="reported", stateDescription="state=accepted 表示受理；state=completed 表示完成。")
    check("reported_with_description", validator.is_valid(package))

    for case in ("duplicate_operation", "duplicate_configuration", "duplicate_requirement",
                 "missing_setting", "optional_setting", "wrong_program_kind", "missing_instructions",
                 "wrong_input_mapping"):
        package = copy.deepcopy(examples["cli"])
        definition = package["definition"]
        invocation = definition["operations"][0]["invocation"]
        if case.startswith("duplicate_"):
            collection = {"duplicate_operation": "operations", "duplicate_configuration": "configuration",
                          "duplicate_requirement": "requirements"}[case]
            definition[collection].append(copy.deepcopy(definition[collection][0]))
        elif case == "missing_setting":
            invocation["program"]["setting"] = "absent"
        elif case == "optional_setting":
            definition["configuration"][0]["required"] = False
        elif case == "wrong_program_kind":
            definition["configuration"][0]["kind"] = "mcp_connection"
        elif case == "missing_instructions":
            definition["instructions"] = "absent.md"
        else:
            invocation["arguments"] = [{"expand": "/missing"}]
        check(case, bool(reference_issues(package, ROOT / "examples" / "cli")))

    for kind, args, expected in [("script", {"text": "你好"}, True), ("script", {"text": 1}, False),
                                 ("cli", {"argv": ["status", "--short"]}, True),
                                 ("cli", {"argv": ["reset", "--hard"]}, False)]:
        business = examples[kind]["definition"]["operations"][0]["parameters"]["schema"]
        check(f"business_{kind}_{expected}", Draft202012Validator(business).is_valid(args) == expected)
    try:
        json.loads('{"id":"first","id":"second"}', object_pairs_hook=unique_object)
    except ValueError:
        check("duplicate_json_key", True)
    else:
        check("duplicate_json_key", False)

    # Exercise standard Schema features needed by current schedule parameters.
    trigger_schema = {
        "type": "object", "required": ["type"],
        "properties": {"type": {"enum": ["once", "interval"]},
                       "minutes": {"type": "integer", "minimum": 1}, "at": {"type": "string"}},
        "oneOf": [{"properties": {"type": {"const": "once"}}, "anyOf": [{"required": ["at"]}, {"required": ["minutes"]}]},
                  {"properties": {"type": {"const": "interval"}}, "required": ["minutes"]}],
    }
    package = copy.deepcopy(examples["script"])
    package["definition"]["operations"][0]["parameters"]["schema"] = trigger_schema
    check("composed_business_schema", validator.is_valid(package))
    for args, expected in [({"type": "once", "at": "10:00"}, True),
                           ({"type": "interval", "minutes": 5}, True),
                           ({"type": "interval", "minutes": 0}, False),
                           ({"type": "once"}, False)]:
        check(f"trigger_{len(checks)}", Draft202012Validator(trigger_schema).is_valid(args) == expected)

    # Plugin/Skill machine manifests, independent of Tool format 1.
    plugin_validator = Draft202012Validator(read_json(ROOT / "plugin.schema.json"))
    skill_validator = Draft202012Validator(read_json(ROOT / "skill.schema.json"))
    mixed = read_json(ROOT / "examples/plugin/plugin.json")
    skill = read_json(ROOT / "examples/plugin/skills/summary/skill.json")
    check("plugin_mixed_manifest", plugin_validator.is_valid(mixed))
    check("skill_optional_dependency", skill_validator.is_valid(skill))
    for kind in ("tool", "skill"):
        single = copy.deepcopy(mixed)
        single["entries"] = [entry for entry in single["entries"] if entry["kind"] == kind]
        check(f"plugin_only_{kind}", plugin_validator.is_valid(single))
    check("plugin_unknown_kind", not plugin_validator.is_valid(read_json(ROOT / "examples/invalid/plugin-kind.json")))
    check("skill_missing_instructions", not skill_validator.is_valid(read_json(ROOT / "examples/invalid/skill-missing-instructions.json")))
    for value, target in ((mixed, plugin_validator), (skill, skill_validator)):
        malformed = copy.deepcopy(value)
        malformed["id"] += "\n"
        check(f"manifest_identifier_newline_{len(checks)}", not target.is_valid(malformed))

    with tempfile.TemporaryDirectory(prefix="desktop-pet-contract-") as temporary:
        work = Path(temporary)
        usage = (ROOT / "examples/script/USAGE.md").read_text(encoding="utf-8")
        script = work / "count.py"
        script.write_text(re.search(r"```python\n(.*?)\n```", usage, re.S).group(1), encoding="utf-8")
        output_schema = examples["script"]["definition"]["operations"][0]["result"]["schema"]
        for value in ("你好abc", "", "e\u0301🙂", "$(touch unexpected); `echo text`"):
            result = subprocess.run([sys.executable, str(script)], input=json.dumps({"text": value}),
                                    capture_output=True, text=True, cwd=work, timeout=10)
            data = json.loads(result.stdout)
            check(f"script_count_{len(checks)}", result.returncode == 0 and not result.stderr
                  and Draft202012Validator(output_schema).is_valid(data) and data["count"] == len(value))
        result = subprocess.run([sys.executable, str(script)], input='{"text":42}',
                                capture_output=True, text=True, cwd=work, timeout=10)
        check("script_invalid_input", result.returncode != 0 and not result.stdout and bool(result.stderr))
        check("script_literal_content", not (work / "unexpected").exists())

        git = shutil.which("git")
        if git is None:
            raise RuntimeError("Git is required for the CLI fixture checks")
        env = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
        env.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull, GIT_TERMINAL_PROMPT="0")
        repo = work / "repo"
        subprocess.run([git, "init", "--quiet", str(repo)], env=env, check=True, capture_output=True, timeout=10)
        for args in (["--version"], ["status", "--short"]):
            result = subprocess.run([git, "--no-pager", "--no-optional-locks", *args], cwd=repo,
                                    env=env, capture_output=True, text=True, timeout=10)
            check(f"cli_{args[0]}", result.returncode == 0 and
                  (result.stdout.startswith("git version ") if args == ["--version"] else result.stdout == ""))
        (repo / "sample.txt").write_text("fixture", encoding="utf-8")
        result = subprocess.run([git, "--no-pager", "--no-optional-locks", "status", "--short"],
                                cwd=repo, env=env, capture_output=True, text=True, timeout=10)
        check("cli_changed_repo", result.returncode == 0 and "?? sample.txt" in result.stdout)

        # Test actual existing playlist query functions with a disposable DB.
        # This is not the CLI entry point or proof of LX Music integration.
        playlist_path = ROOT.parents[1] / "core/ai/skills/lx-music-skill/scripts/playlist.py"
        namespace = runpy.run_path(str(playlist_path))
        database = work / "playlist.db"
        with sqlite3.connect(database) as connection:
            connection.executescript("""
                CREATE TABLE my_list (id TEXT, name TEXT, source TEXT, sourceListId TEXT, position INTEGER);
                CREATE TABLE my_list_music_info (id TEXT, name TEXT, singer TEXT);
                CREATE TABLE my_list_music_info_order (listId TEXT, musicInfoId TEXT, "order" INTEGER);
                INSERT INTO my_list VALUES ('fixture', '示例歌单', 'wy', '123', 0);
                INSERT INTO my_list_music_info VALUES ('song', '示例歌曲', '示例歌手');
                INSERT INTO my_list_music_info_order VALUES ('fixture', 'song', 0);
            """)
        namespace["get_playlists"].__globals__["LX_DATA"] = str(database)
        check("existing_playlist_array", namespace["get_playlists"]() == [{
            "id": "fixture", "name": "示例歌单", "source": "wy", "sourceId": "123",
            "playUrl": "lxmusic://songlist/play/wy/123"}])
        check("existing_playlist_songs", namespace["get_playlist_songs"]("fixture") == [
            {"name": "示例歌曲", "singer": "示例歌手"}])
        check("existing_playlist_empty", namespace["get_playlist_songs"]("missing") == [])

    print(json.dumps({"checks": len(checks), "passed": sum(item["passed"] for item in checks),
                      "results": checks}, ensure_ascii=False, indent=2))
    return 0 if all(item["passed"] for item in checks) else 1


if __name__ == "__main__":
    sys.exit(main())
