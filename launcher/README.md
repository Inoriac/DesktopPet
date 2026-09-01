# DesktopPet 启动器 (Python)

PySide6 + PySide6-Fluent-Widgets 实现的 Fluent Design 启动器。
负责配置管理 + 指定角色启动，通过命令行参数 `--config` / `--pet` 唤起 C++ 核心。

详见仓库根 `前端修改计划.md`。

## 运行

```powershell
py -3 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r launcher\requirements.txt
.\.venv\Scripts\python.exe launcher\main.py
```

> 本项目未使用 PySide6-Addons（WebEngine/3D），只装 Essentials 即可，省 ~300MB 下载。

## 前置

- C++ 核心须已构建到 `build/`；支持单配置产物和 `build/Release`、`build/Debug` 等多配置产物。
- 启动器以 `--config <绝对路径> --pet <角色名> --profile-id <uuid>` 启动核心。
- Windows 下配置保存在 `%APPDATA%\Desktop Pet Team\Desktop Pet\launch_config.json`。
  AI 页面可显式保存配置，launcher 下次启动时会自动恢复用户字段。

## 主题共享

启动器启动时 `setOrganizationName/ApplicationName` 逐字对齐 C++ `main.cpp`，
经同名 QSettings（键 `ui/theme`）与 C++ 共享主题，无需配置文件中转。见计划 §3.1。

## 文件

| 文件 | 作用 |
|------|------|
| `main.py` | 主入口：FluentWindow 框架 + 导航 + 启动按钮 + 主题共享 |
| `app_state.py` | 各页配置的运行期状态（dataclass） + 字段映射 settings dict |
| `config_loader.py` | 模板加载 + 用户字段覆盖 + 导出 launch_config.json |
| `pet_registry.py` | 读写 C++ `Pet` 注册表（AppData/pets.json） |
| `pages/` | 宠物/AI/语音/气泡/高级/关于 各设置页 |
| `pages/_ui.py` | Fluent 页面、分区和设置行布局助手 |
| `requirements.txt` | 依赖清单 |
