"""Read and atomically upgrade the launcher-owned pet profile registry."""

from __future__ import annotations

import json
import os
import secrets
import shutil
import sqlite3
import uuid
from dataclasses import dataclass, replace
from typing import List, Optional

from PySide6.QtCore import QLockFile

from atomic_write import atomic_write_text


ORG_NAME = "Desktop Pet Team"
APP_NAME = "Desktop Pet"
_LOCK_TIMEOUT_SECONDS = 5.0
_DEFAULT_NAME = "Milltina"
_DEFAULT_MODEL_PATH = "assets/models/milltina/model/milltina.gltf"
_PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_WRITABLE_DIR_CACHE: dict[str, str] = {}


class PetRegistryError(RuntimeError):
    """The registry cannot be upgraded without risking profile identity."""


@dataclass(frozen=True)
class PetEntry:
    name: str
    model_path: str
    profile_id: str

    def to_dict(self) -> dict:
        return {
            "name": self.name,
            "modelPath": self.model_path,
            "profileId": self.profile_id,
        }


def _sqlite_db_healthy(path: str) -> bool:
    try:
        conn = sqlite3.connect(path, timeout=1.0)
        try:
            row = conn.execute("PRAGMA quick_check").fetchone()
            return bool(row and row[0] == "ok")
        finally:
            conn.close()
    except sqlite3.Error:
        return False


def _copy_sqlite_bundle(src_db: str, dst_db: str) -> None:
    for suffix in ("", "-wal", "-shm"):
        candidate = dst_db + suffix
        if os.path.exists(candidate):
            try:
                os.unlink(candidate)
            except OSError:
                return
    shutil.copy2(src_db, dst_db)
    for suffix in ("-wal", "-shm"):
        src_sidecar = src_db + suffix
        if os.path.isfile(src_sidecar):
            shutil.copy2(src_sidecar, dst_db + suffix)
    try:
        conn = sqlite3.connect(dst_db, timeout=1.0)
        try:
            conn.execute("PRAGMA wal_checkpoint(TRUNCATE)")
        finally:
            conn.close()
    except sqlite3.Error:
        pass


def _seed_fallback_dir(primary: str, fallback: str) -> None:
    os.makedirs(fallback, exist_ok=True)
    legacy_launcher_cfg = os.path.join(
        _PROJECT_ROOT, "runtime", "launcher", "launch_config.json")
    fallback_cfg = os.path.join(fallback, "launch_config.json")
    if os.path.isfile(legacy_launcher_cfg) and not os.path.isfile(fallback_cfg):
        try:
            shutil.copy2(legacy_launcher_cfg, fallback_cfg)
        except OSError:
            pass
    if not os.path.isdir(primary):
        return
    for root, dirs, files in os.walk(primary):
        rel = os.path.relpath(root, primary)
        target_root = fallback if rel == "." else os.path.join(fallback, rel)
        try:
            os.makedirs(target_root, exist_ok=True)
        except OSError:
            continue
        for name in files:
            if (name.endswith(".lock") or name.endswith("-wal")
                    or name.endswith("-shm")):
                continue
            src_path = os.path.join(root, name)
            dst_path = os.path.join(target_root, name)
            is_sqlite = name.endswith((".db", ".sqlite", ".sqlite3"))
            if not os.path.exists(dst_path):
                try:
                    if is_sqlite:
                        _copy_sqlite_bundle(src_path, dst_path)
                    else:
                        shutil.copy2(src_path, dst_path)
                except OSError:
                    pass
            elif is_sqlite and not _sqlite_db_healthy(dst_path):
                try:
                    _copy_sqlite_bundle(src_path, dst_path)
                except OSError:
                    pass


def _app_data_dir() -> str:
    if os.name == "nt":
        base = os.environ.get("APPDATA") or os.path.expanduser("~")
    elif sys_platform() == "darwin":
        base = os.path.expanduser("~/Library/Application Support")
    else:
        base = os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")
    primary = os.path.join(base, ORG_NAME, APP_NAME)
    cached = _WRITABLE_DIR_CACHE.get(primary)
    if cached is not None:
        return cached
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_BINARY", 0)
    try:
        os.makedirs(primary, exist_ok=True)
        probe = os.path.join(primary, f".dp-probe-{secrets.token_hex(8)}.tmp")
        descriptor = os.open(probe, flags, 0o600)
        os.close(descriptor)
        os.unlink(probe)
        _WRITABLE_DIR_CACHE[primary] = primary
        return primary
    except OSError:
        fallback = os.path.join(_PROJECT_ROOT, "runtime", "appdata")
        fallback_tmp = os.path.join(fallback, "tmp")
        os.makedirs(fallback_tmp, exist_ok=True)
        current_tmp = os.environ.get("TEMP") or os.environ.get("TMP") or ""
        tmp_writable = False
        if current_tmp:
            try:
                probe_tmp = os.path.join(
                    current_tmp, f".dp-probe-{secrets.token_hex(8)}.tmp")
                fd = os.open(probe_tmp, flags, 0o600)
                os.close(fd)
                os.unlink(probe_tmp)
                tmp_writable = True
            except OSError:
                tmp_writable = False
        if not tmp_writable:
            os.environ["TMP"] = fallback_tmp
            os.environ["TEMP"] = fallback_tmp
        _seed_fallback_dir(primary, fallback)
        _WRITABLE_DIR_CACHE[primary] = fallback
        return fallback


def sys_platform() -> str:
    import sys
    return sys.platform


def pets_json_path() -> str:
    return os.path.join(_app_data_dir(), "pets.json")


def _is_canonical_uuid(value: object) -> bool:
    if not isinstance(value, str):
        return False
    try:
        return str(uuid.UUID(value)) == value
    except (ValueError, AttributeError):
        return False


def _read_raw(path: str) -> list[dict]:
    try:
        with open(path, "r", encoding="utf-8") as handle:
            data = json.load(handle)
    except (OSError, json.JSONDecodeError) as error:
        raise PetRegistryError(f"failed to read pet registry: {error}") from error
    if not isinstance(data, list):
        raise PetRegistryError("pet registry root must be an array")
    if not data:
        raise PetRegistryError("pet registry must contain at least one profile")
    if not all(isinstance(item, dict) for item in data):
        raise PetRegistryError("pet registry entries must be objects")
    return data


def _validate_entries(data: list[dict], allow_missing_profile_id: bool) -> List[PetEntry]:
    entries: List[PetEntry] = []
    seen_names: set[str] = set()
    seen_ids: set[str] = set()
    for item in data:
        name = item.get("name")
        model_path = item.get("modelPath")
        profile_id = item.get("profileId", "")
        if not isinstance(name, str) or not name.strip():
            raise PetRegistryError("pet registry entry has an invalid name")
        if not isinstance(model_path, str) or not model_path.strip():
            raise PetRegistryError(f"pet profile {name!r} has an invalid modelPath")
        if name in seen_names:
            raise PetRegistryError(f"duplicate pet name: {name}")
        seen_names.add(name)
        if not profile_id and allow_missing_profile_id:
            entries.append(PetEntry(name, model_path, ""))
            continue
        if not _is_canonical_uuid(profile_id):
            raise PetRegistryError(f"pet profile {name!r} has an invalid profileId")
        if profile_id in seen_ids:
            raise PetRegistryError(f"duplicate profileId: {profile_id}")
        seen_ids.add(profile_id)
        entries.append(PetEntry(name, model_path, profile_id))
    return entries


class _RegistryLock:
    def __init__(self, path: str):
        self.lock = QLockFile(path)
        self.lock.setStaleLockTime(30000)
        self.acquired = False

    def __enter__(self):
        if not self.lock.tryLock(int(_LOCK_TIMEOUT_SECONDS * 1000)):
            raise PetRegistryError(
                f"failed to acquire pets.json.lock: {self.lock.error()}")
        self.acquired = True
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        if self.acquired:
            self.lock.unlock()
            self.acquired = False


def _atomic_write(path: str, entries: List[PetEntry]) -> None:
    directory = os.path.dirname(path)
    os.makedirs(directory, exist_ok=True)
    payload = json.dumps(
        [entry.to_dict() for entry in entries],
        ensure_ascii=False, indent=2) + "\n"
    atomic_write_text(path, payload, prefix="pets.json.")


def _load_or_upgrade_locked(path: str) -> List[PetEntry]:
    if not os.path.exists(path):
        entries = [PetEntry(_DEFAULT_NAME, _DEFAULT_MODEL_PATH, str(uuid.uuid4()))]
        _atomic_write(path, entries)
        return entries

    entries = _validate_entries(_read_raw(path), allow_missing_profile_id=True)
    if any(not entry.profile_id for entry in entries):
        entries = [
            entry if entry.profile_id else replace(entry, profile_id=str(uuid.uuid4()))
            for entry in entries
        ]
        _validate_entries([entry.to_dict() for entry in entries], allow_missing_profile_id=False)
        _atomic_write(path, entries)
    return entries


def load_pets() -> List[PetEntry]:
    path = pets_json_path()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if os.path.exists(path):
        entries = _validate_entries(_read_raw(path), allow_missing_profile_id=True)
        if all(entry.profile_id for entry in entries):
            return entries
    with _RegistryLock(path + ".lock"):
        return _load_or_upgrade_locked(path)


def find_pet(name: str) -> Optional[PetEntry]:
    return next((entry for entry in load_pets() if entry.name == name), None)


def rename_pet(profile_id: str, new_name: str) -> PetEntry:
    if not _is_canonical_uuid(profile_id):
        raise PetRegistryError("invalid profileId")
    if not isinstance(new_name, str) or not new_name.strip():
        raise PetRegistryError("new pet name must not be empty")
    path = pets_json_path()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with _RegistryLock(path + ".lock"):
        entries = _load_or_upgrade_locked(path)
        if any(entry.name == new_name and entry.profile_id != profile_id for entry in entries):
            raise PetRegistryError(f"duplicate pet name: {new_name}")
        renamed: Optional[PetEntry] = None
        updated: List[PetEntry] = []
        for entry in entries:
            if entry.profile_id == profile_id:
                renamed = replace(entry, name=new_name)
                updated.append(renamed)
            else:
                updated.append(entry)
        if renamed is None:
            raise PetRegistryError(f"profile not found: {profile_id}")
        _atomic_write(path, updated)
        return renamed


def list_pet_names() -> List[str]:
    return [entry.name for entry in load_pets()]


def has_pet(name: str) -> bool:
    return find_pet(name) is not None
