# 语音服务空闲卸载（保活窗口）实现记录

> 状态：方案已定，待实现。计划详见 `~/.codefuse/engine/cc/plans/tools-voice-python-core-voice-servive-swirling-mist.md`。

## 背景

桌宠语音合成走 `genie-tts`（ONNX Runtime，~0.5–2GB 常驻）。`VoiceSynthesisService`（`core/voice/voice_synthesis_service.cpp`）
用 `QProcess` spawn `tools/voice/genie_worker.py`，stdin/stdout JSON-lines 通信。模型在子进程 `configure` 时加载一次、
随桌宠整个生命周期常驻；`preloadOnStart` 默认 `true`，开机即占满且永不释放。

**目标**：保留 GENIE 音色与情感（不换模型、不上云），仅改生命周期——空闲超保活窗口就关停子进程释放内存，下次 `speak()` 复用
现有 `ensureStarted()` 重新拉起（静置后首句数秒冷启动，已接受）。

## 已定决策

- 方向 **A：带保活窗口的空闲卸载**。不引入后端抽象；上云(edge-tts)/换轻量模型(sherpa-onnx/Piper)留作后续可选 `backend` 方向
  （两者都会改音色且情感表现明显变弱）。
- 保活窗口**可配置，默认 300s（5 分钟）**；`0` = 禁用（退回常驻旧行为）。
- `preloadOnStart` 默认翻为 `false`（开机不再预加载，首次 `speak` 才拉起）。

## 改动清单（5 个文件）

### 1. `include/ai_types.h` — `VoiceConfig`（`:102-123`）
- `:108` `bool preloadOnStart = true;` → `false`
- `:122` `int maxTextChars = 350;` 后新增 `int voiceIdleKeepAliveSec = 300;`（0 = 永不空闲卸载）

### 2. `core/configLoader/config_manager.cpp` — `parseVoiceConfig`（`:54-109`）
- `:65` `cfg.preloadOnStart = voiceObj.value("preloadOnStart").toBool(true);` → `.toBool(false)`
- `:107` 后新增：`cfg.voiceIdleKeepAliveSec = clampInt(voiceObj.value("voiceIdleKeepAliveSec").toInt(300), 0, 3600);`（复用 `:37` `clampInt`）

### 3. `config/default_common_config.json` — `ai.voice` 块（`:93-122`）
- `"preloadOnStart"` 默认改 `false`
- 新增 `"voiceIdleKeepAliveSec": 300`

### 4. `core/voice/voice_synthesis_service.h`
- 顶部加 `#include <QTimer>`
- private 方法（`warnOnce` 之后）：`void scheduleIdleTeardown();`、`void cancelIdleTeardown();`、`void onIdleTimeout();`
- private 成员（`m_warnedKeys` 之后）：`QTimer* m_idleTimer = nullptr;`

### 5. `core/voice/voice_synthesis_service.cpp`
- **构造函数（`:24-25`）**：建单次定时器 `m_idleTimer = new QTimer(this); setSingleShot(true); setTimerType(CoarseTimer); connect(timeout→onIdleTimeout)`。
- **`scheduleIdleTeardown()`**：守卫 `keepAlive<=0` / `inFlight||configureInFlight||!pendingText.isEmpty()` / `!m_process||NotRunning` → 否则 `m_idleTimer->start(keepAlive*1000)`。
- **`cancelIdleTeardown()`**：`if (m_idleTimer && m_idleTimer->isActive()) m_idleTimer->stop();`
- **`onIdleTimeout()`**：复校验空闲守卫 → `qDebug` → `stop();`
- **调度点（`startNextPending()` 之后，仅在转为空闲的事件）**：
  - `configured` 分支（`:267` 后）`scheduleIdleTeardown();`
  - `speech_finished/speech_skipped` 分支（`:280` 后）
  - `error` 分支（`:298` 后）
- **取消点**：
  - `speak()`（`:55` 后第一行）`cancelIdleTeardown();`
  - `stop()`（`:85` 第一行，`:86` 之前）`cancelIdleTeardown();`
  - `handleProcessFinished`（`:304`）/`handleProcessError`（`:316`）末尾各加 `cancelIdleTeardown();`
- **`setConfig()`（可选）**：`:34` 后若定时器在跑则 `scheduleIdleTeardown();` 重新按新间隔排程。
- **不改** `configToJson()`/`configKey()`：keepalive 是父进程侧生命周期参数，不发给子进程、不进 configKey（避免调保活时长无谓重启）。

## 边界情况

| 场景 | 处理 |
|---|---|
| 定时器在跑时来 `speak()` | `speak()` 先取消；worker 仍活则 `ensureStarted()` 空操作 |
| configKey 变更重启 | `stop()` 取消；重拉起后 `configured` 重排 |
| `voiceIdleKeepAliveSec==0` | `scheduleIdleTeardown` 早退，常驻旧行为 |
| 子进程崩溃时定时器在跑 | finished/error 末尾取消 |
| closeEvent/析构二次 `stop()` | `stop()` 首行取消；二次在 `:95` 早退，无泄漏 |
| 内存真释放 | 模型全在子进程地址空间，`stop()` kill 链由 OS 回收 |
| 连发回复抖动 | 单次定时器每次完成重排；in-flight/pending 守卫防止中途卸载 |
| configured 但从未 speak | configured 分支无 pending → 直接调度 |

## 验证（端到端手测）

1. `config`：`ai.voice.enabled=true`、`voiceIdleKeepAliveSec=30`、`preloadOnStart=false`，开启某来源（如 `assistant`）。
2. 启动 → `ps aux | grep genie_worker` 确认无 Python 进程（未预加载）。
3. 触发一次回复 → 进程出现、发声正常。
4. 发声结束 → 进程仍在（定时器在跑）。
5. 静置 30s → 进程在 ~30s 消失；日志见 `[Voice] idle teardown after 30 s`。
6. 再触发 → 进程重新拉起（冷启动）、发声正常。
7. 设 `voiceIdleKeepAliveSec=0` → 触发后常驻不退（旧行为）。
8. 30s 保活下 10s 内连发 3 次 → burst 中途不卸，最后一次完成后 30s 卸。
9. 关闭桌宠 → 无残留进程、无 QTimer 警告。
10. `cmake --build build` 编译通过（无现成 voice 测试桩，以手测为准）。

## 后续可选方向（本次不做）

- **上云 edge-tts**：常驻 ~0，但需联网、文本外发（隐私）、音色变、商用 ToS 灰区。
- **轻量本地 sherpa-onnx/Piper**：常驻降到百 MB 级、有 C++ API 可去掉 Python sidecar 纯 C++ 推理；但音色变（非 feibi）、情感表现明显变弱。
- 欲启用任一，建议先把 `backend` 字段抽象为 `IVoiceBackend` 接口，GENIE 收敛为 `GenieBackend`，再新增对应后端。