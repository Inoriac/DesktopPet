"""宠物角色注册表读写。

与 C++ entity/pet.* 对齐：数据存放于 QStandardPaths::AppDataLocation 下的
pets.json，格式为 JSON 数组 [{"name": ..., "modelPath": ...}]。

注意：路径与 C++ 端严格一致（组织名/应用名相同才解析到同一文件），见 main.py。
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass
from typing import List

# 必须与 C++ 端 main.cpp 逐字一致，否则 QStandardPaths 路径不同文件不同。
ORG_NAME = "Desktop Pet Team"
APP_NAME = "Desktop Pet"


@dataclass
class PetEntry:
    name: str
    model_path: str

    def to_dict(self) -> dict:
        return {"name": self.name, "modelPath": self.model_path}


def _app_data_dir() -> str:
    """返回 C++ QStandardPaths::AppDataLocation 对应目录（macOS/Win/Linux 通用推断）。"""
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


def load_pets() -> List[PetEntry]:
    """读取角色列表。文件缺失或格式错误时返回空列表（C++ 端首启会自建默认 Milltina）。"""
    path = pets_json_path()
    if not os.path.exists(path):
        return []
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError):
        return []
    if not isinstance(data, list):
        return []
    pets: List[PetEntry] = []
    for obj in data:
        if isinstance(obj, dict) and obj.get("name") and obj.get("modelPath"):
            pets.append(PetEntry(name=obj["name"], model_path=obj["modelPath"]))
    return pets


def list_pet_names() -> List[str]:
    return [p.name for p in load_pets()]


def has_pet(name: str) -> bool:
    return any(p.name == name for p in load_pets())