# 🐾 Desktop Pet (3D桌宠)




一个基于 C++ 和 Qt 6 开发的高性能、高可定制化的桌面宠物系统。
本项目旨在提供一个生动、智能且具有“灵魂”的桌面伴侣，支持 GLTF 模型热加载、复杂的动画状态机、情绪系统以及丰富的桌面交互功能。

> **⚠️ 重要声明 / Important Notice**
> 
> 本项目基于 **MateEngine** 的部分核心逻辑与资源开发。
> 遵循 **MateEngine Pro License (v2.1)** 协议。
> **严禁商用**。仅供学习、研究与非商业用途。
> This project is strictly **Non-Commercial**.

---

## ✨ 功能特性 (Features)

### ✅ 已实现 / Implemented

* **核心引擎**
  * **GLTF 模型热加载**：支持运行时动态加载 `.gltf` 模型文件。
  * **动画状态机 (ASM)**：基于状态机的复杂动画调度系统，支持平滑插值过渡。
  * **性能优化**：支持帧率、渲染质量、窗口大小的多级动态设置。
* **交互系统**
  * **触摸反馈**：基于身体部位（头部、身体等）的覆盖层检测，支持不同的触摸反应。
  * **窗口互动**：支持拖动宠物窗口，具备贴附屏幕边缘等特性。
* **生活功能**
  * **智能闹钟**：不仅仅是提醒。宠物有一定概率（基于性格）“忘记”提醒，并在事后表现出懊恼或敷衍的反应。
  * **统计系统**：记录互动次数、情绪变化、运行时间等数据，让宠物的成长有迹可循。
* **个性化**
  * **自定义主题**：支持修改 UI 主题颜色与样式。
  * **配置热加载**：无需重启即可应用部分功能开关与参数调整。

### 🚧 开发中 / In Progress

* **情绪系统**：包含开心、害羞、难过、愤怒四种基础情绪，随互动衰减或增强，影响对话与动作。
* **聊天系统**：
  * 本地简易对话（基于情绪的语义映射）。
  * 接入 AI 大模型 API 进行自由对话。
* **物理系统**：全局物理模拟（重力、头发飘动等）。
* **视觉增强**：
  * **注视跟随**：头部、脊柱与眼部的鼠标跟随系统。
  * **特殊节日皮肤**：圣诞帽、南瓜头等节日限定饰品自动渲染。
* **多媒体**：随机播放网易云/QQ音乐歌单。
* **多实例支持**：桌面上同时存在多只宠物。

---

## 🛠️ 技术栈 (Tech Stack)

* **语言**: C++ 20
* **UI 框架**: Qt 6 (Widgets & Core)
* **构建系统**: CMake
* **图形/模型**: OpenGL, TinyGLTF
* **音频**: OpenAL / Qt Multimedia

---

## 📂 项目结构 (Project Structure)

```text
Desktop-Pet/
├── assets/             # 资源文件 (模型, 动画, 配置, 图标)
├── config/             # 配置文件 (状态机定义, 个性设置)
├── core/               # 核心逻辑 (动画控制, 行为树, 事件处理)
├── engine/             # 渲染引擎 (OpenGL封装, 模型加载, 音频)
├── entity/             # 实体逻辑 (宠物类, 个性化数据)
├── ui/                 # 界面实现 (主窗口, 托盘, 菜单)
├── statistic/          # 数据统计模块
└── third_party/        # 第三方库 (TinyGLTF等)
```

---

## 🚀 快速开始 (Getting Started)

### 环境要求

* C++ 编译器 (支持 C++20, 如 MSVC 2019+ 或 MinGW 11+)
* Qt 6.x SDK
* CMake 3.20+

### 构建步骤

1. **克隆仓库**
   
   ```bash
   git clone https://github.com/YourUsername/Desktop-Pet.git
   cd Desktop-Pet
   ```

2. **配置 CMake**
   
   ```bash
   mkdir build
   cd build
   cmake .. -DCMAKE_PREFIX_PATH="path/to/your/Qt/6.x.x/mingw_64"
   ```

3. **编译**
   
   ```bash
   cmake --build .
   ```

4. **运行**
   确保 `assets` 文件夹与可执行文件在同一级目录，或在配置中正确指定路径。

---

## ⚙️ 配置说明 (Configuration)

项目包含两份主要配置文件：

1. **`default_common_config.json`**: 系统默认配置，用于初始化或恢复出厂设置。
2. **`userConfig.json`** (生成): 用户自定义配置，保存界面风格、功能开关等。

---

## 📄 协议与版权 (License & Credits)

### Project License

本项目遵循 **MateEngine Pro License (v2.1)**。
查看完整的 [LICENSE](LICENSE.md) 文件。

### Attribution

* **Developer**: Inoriac
* **Original Engine Logic & Inspiration**: [Mate-Engine](https://github.com/shinyflvre/Mate-Engine) by Johnson Jason.
* **Assets (Animations/Visuals)**: Copyright © 2025 **Shiny**.
  * *Note: Assets are used under a non-commercial license and strictly copyrighted.*

**严禁将本项目用于任何商业用途（包括但不限于 Steam、itch.io 销售或付费订阅）。**
**Strictly prohibited for commercial use.**
