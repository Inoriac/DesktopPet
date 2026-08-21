"""Read and atomically upgrade the launcher-owned pet profile registry."""

from __future__ import annotations

import json
import os
import tempfile
import uuid
from dataclasses import dataclass, replace
from typing import List, Optional

from PySide6.QtCore import QLockFile


ORG_NAME = "Desktop Pet Team"
APP_NAME = "Desktop Pet"
_LOCK_TIMEOUT_SECONDS = 5.0
_DEFAULT_NAME = "Milltina"
_DEFAULT_MODEL_PATH = "assets/models/milltina/model/milltina.gltf"


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


def _app_data_dir() -> str:
    if os.name == "nt":
        base = os.environ.get("APPDATA") or os.path.expanduser("~")
    elif sys_platform() == "darwin":
        base = os.path.expanduser("~/Library/Application Support")
    else:
        base = os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")
    return os.path.join(base, ORG_NAME, APP_NAME)


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
    descriptor, temp_path = tempfile.mkstemp(prefix="pets.json.", suffix=".tmp", dir=directory)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
            json.dump([entry.to_dict() for entry in entries], handle,
                      ensure_ascii=False, indent=2)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temp_path, path)
        try:
            directory_fd = os.open(directory, os.O_RDONLY)
            try:
                os.fsync(directory_fd)
            finally:
                os.close(directory_fd)
        except OSError:
            pass
    except BaseException:
        try:
            os.unlink(temp_path)
        except FileNotFoundError:
            pass
        raise


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
