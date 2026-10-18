# Windows 连续交互、记忆与关闭渲染验收

## 结论

2026-10-18 21:07–21:27（北京时间），基线 `1a5ad3e`，重新构建 Desktop_Pet 后在真实 Windows / Qt OpenGL 窗口试运行。包含三段桌宠会话、重启、两次短时渲染探针；不是整段无中断或长期稳定性压测。使用 `gemini-3.8-flash-high`，测试配置启用对话和记忆相关模型角色，关闭语音与屏幕识别。正式模型配置未修改。

连续聊天、记忆写入/整理/重启检索、隐藏后聊天以及停止/恢复绘制已有成功证据。但旧记忆迁移存在归属风险，且真实对话出现三次连接关闭，因此不能判定整体交互完全符合预期。

## 连续交互

通过应用正式 LauncherChatClient → 本地认证管道 → Desktop_Pet / AIBrain 发消息，真实 UI 用 Windows computer-use 观察和操作。没有使用模拟模型回答。

- 首轮告知称呼小岚、无糖桂花乌龙、安静阅读、书签青柠灯塔，回答准确。
- 询问阅读前休息建议，并要求不创建提醒：回答引用前文偏好，没有创建提醒。
- 将饮品纠正为无糖茉莉绿茶：回答采用新偏好，保留书签及安静阅读习惯。这里证明会话内纠正有效，不据此宣称跨日冲突记忆合并已验证。
- 询问未提供的银行卡号：明确回答不知道，没有生成号码。
- 隐藏形象后询问状态：模型实际调用形象查询工具，报告 `visible:false`，后台正常回复。
- 试运行中三次正式请求返回 `LLM network error (code 2): Connection closed`；后续请求自行恢复，单独端点探针也成功。没有足够证据将断连归因于服务器或 Qt 客户端中的某一方。
- UI 会弹出“模型服务暂时不可用，请检查网络或模型配置后重试”。但聊天状态/历史中仅保留已完成的用户消息，没有相应失败回复项，历史回看与重试体验不清晰。错误弹窗关闭后角色仍正常显示。

未验证真实外部账号发言、图片识别、长时间无人值守的自主决策或数小时稳定性；试验期间没有向其他人发送消息。

## 记忆证据

独立验收 profile：`3c4cd805-8a6b-4437-9f93-5845a067c1c8`，角色名 `Milltina-Interaction-Trial`。

1. 输入“补充一条请长期记住：我的测试盆栽叫雾蓝松果，放在北侧窗台，每周三浇水”。模型口头说长期记住，但数据库最初只产生 `short_term / hippocampus`，不能将口头承诺当作长期记忆落库。
2. 输入以“记住：”开头的饮品、阅读及书签偏好，数据库写入 `preference / user_explicit / rule_v1`，置信度 0.98。
3. 通过真实右键菜单“整理记忆”触发整理。盆栽短期条目变为 `consolidated`，产生 `semantic / daydream` 条目：“主人的测试盆栽名为‘雾蓝松果’，放置在北侧窗台，每周三浇水。”原条目保留 `consolidated_into`，新条目保留 `source_memory_ids`。
4. 正常退出后，备份验收角色的 runtime 数据库并移除其中验收聊天消息；保留独立 `memory.db`。重启后的聊天接口确认消息列表为空。旧 JSON 历史文件跨盘移动失败，文件仍在原处，但已导入标记使其未重新载入；空消息状态及真实请求内容均核验。
5. 无历史的新请求中，宿主明确注入上述 `semantic` 与 `preference` 相关记忆，而用户问题没有提供答案。前两次请求断连；第三次成功准确回答盆栽名、位置、周三浇水、无糖茉莉绿茶、安静阅读、青柠灯塔。

这证明本样本的持久化、整理、重启检索和模型使用记忆有效；不代表自动整理延迟、遗忘、跨人物隔离或所有自然语言记忆指令都已验收。

## 发现：旧记忆归属不明仍发生关联迁移

首次从项目根目录启动验收角色时，已有旧 `runtime/memory/memory.db`，注册表含多个角色。ProfileDataMigrator 判为 Ambiguous，返回旧库路径，运行模式为 degraded。随后聊天副作用队列仍运行归属迁移，将旧库中原先没有归属的条目关联到新验收 profile，并将六条本次测试记录写入旧库。

证据：旧库新增的 237 条 attribution 均为本次新 UUID，创建时间从 13:07:38Z 开始；迁移表也记录该 UUID。源码链路为：

- `core/ai/runtime/profile_data_migrator.cpp`：Ambiguous 返回 legacyResult，指向旧库。
- `core/ai/runtime/agent_bootstrap.cpp`：将返回路径交给 initializeStorage。
- `core/ai/chat/chat_side_effect_queue.cpp`：migrateNextBatch 无归属确认门槛地调用 migrateAttribution。
- `core/ai/memory/memory_attribution_service.cpp`：无 attribution 的旧条目赋予当前 profileId。

这是需要优先修复的归属问题；“降级”没有阻止这条迁移路径。建议在归属未确认时禁止归属迁移及向共享旧库写入，明确提示用户选择旧记忆归属，或者让新角色使用独立空库。本轮仅验证与记录，未改动生产逻辑。

为继续验证，通过独立工作目录（仅 junction 复用 assets/config，目录中没有旧记忆库）启动同一验收 profile，运行模式变为 running，独立 memory.db 正常生成。

清理时已将六条测试记忆及关联归属记录归档，然后从旧库移除；撤回本次 UUID 的 237 条 attribution 及迁移游标，保留原记忆正文。SQLite integrity_check 为 ok。清理前恢复备份仅保存在本地忽略目录，不提交。验收角色的独立记忆库保留供追查，测试注册项已移除。

## 关闭与恢复渲染

- 在真实桌宠右键菜单点击“隐藏形象（托盘可恢复）”，桌宠窗口消失；模型查询返回 visible=false，进程继续响应聊天。
- 隐藏前累计 CPU 约 38.94 秒，随后约一分多钟只增长到 40.45 秒；这里只作辅助观察，不作为精确性能基准。
- 自动化工具未暴露 Windows 托盘入口，因此没有将“实际点击托盘恢复”标成通过。
- 另外编译独立 RenderTrial.exe：复用当前生产对象文件、实际 PetWindow/RenderViewport/OpenGL 和模型，只在测试 main 中定时调用 hide/show（与生产 setAvatarVisible 使用同一路径），检查可见性、renderingEnabled 和 frameSwapped。没有修改正式 Desktop_Pet.exe。
- 显示约 3 秒：visible=1、renderingEnabled=1，183 帧；隐藏后排除 0.5 秒过渡，再观察 2.5 秒：visible=0、renderingEnabled=0，新增 0 帧；恢复约 3 秒：两者均为 1，新增 189 帧，退出码 0。恢复截图有角色输出，但抓取到的是侧躺且部分肢体越出画面的姿态；不能据此宣称所有动作的姿态和构图正常。交互窗口另有恢复到站立状态的观察记录，是否为动画预期动作/镜头裁切需进一步专项核对。

结论：关闭渲染确实停止持续绘制；后台聊天可继续；同进程恢复绘制通过。Windows 托盘点击入口的恢复操作仍未人工/自动点击验收。

## 本地复核产物

- `build/interaction-trial/states.jsonl`：分会话聊天状态快照，包含空历史和最终召回回答。
- `build/interaction-trial/memory_items.json`、`final-memory-inspection.txt`：验收角色记忆证据。
- `build/interaction-trial/sandbox/log/ai_calls.jsonl`：实际请求的相关记忆及三次断连记录。
- `build/interaction-trial/render-probe-final.log`：帧数与状态，末尾 RENDER_PROBEPASS。
- `build/interaction-trial/render-restored.png`：恢复后的真实 OpenGL 截图。
- `build/interaction_trial.py`、`build/interaction-trial/build_render_probe.py`：本地验收脚本，不是生产入口。

生产源码无本轮修改；未提交或推送。验收启动配置中的 API 凭据和会话令牌在结束后已移除。
