# 形象行为配置

形象使用 `ConfigManager::getStateMachinePath()` 指向的 JSON（发布默认值为 `config/animation_state_machine.json`）。修改后重启桌宠生效。本期不支持运行时换形象或热加载规则。气泡与语音不在形象渲染契约内。

`avatar.id` / `avatar.description` 描述当前外观及形象设定；它们进入当前模型上下文，不修改 profileId、人格数据和记忆。状态标识可任意命名，不要求包含 Idle、Sleeping 或其他固定状态。

## 状态和动作

- `defaultState`：初态，必须在 `states` 中声明且不能被禁用。
- `states[].name` / `description`：标识及供模型理解的含义。
- `loop`：是否循环。
- `requestable`：模型是否可直接申请此状态，默认 true。交互、情绪与调试仍须遵守转换和保护规则。
- `disabled`：禁用这个状态，所有来源都不能申请，也不能通过 `via` 间接进入。
- `clipOptions`：候选动作，元素为 `{ "clipName": "资源名", "weight": 1, "interruption": "finish_cycle" }`。weight 为正数；interruption 可省略以继承状态规则。为空时由适配器从该状态的资源目录发现。
- `folder` / `clipMatch`：当前 3D 适配器的资源发现设置；另一个适配器可使用自己的资源发现方式。
- `fallbackState`：当此状态没有可用资源时，允许尝试的替代状态。必须引用另一已声明状态；反馈包含 requested_state、实际 target_state 和 substituted。替代也要遵守可达关系与策略。不自动递归替代。

查询 `get_avatar_capabilities` 可看到实际可用动作。缺少某些资源只让相应状态不可用；不会阻止普通聊天。初态不可播放时，状态仍是配置初态，playing=false，并报告原因，不伪装成播放成功。

## 转换与保护

`transactions` 的元素为 `{ "fromState": "A", "toState": "B", "condition": "event", "via": ["X", "Y"] }`。

- fromState 为 `*` 时是通用边，具体来源边优先；同一来源与目标存在具体边时，它会覆盖通用边，包括其是否自动转换的设置。
- via 可省略；列出的状态按顺序完整播放，再进入目标。每段必须是有限、非循环动作。
- 空 condition 表示当前非循环状态播完后的自动转换；优先选择具体来源的自动边，其次选择未被覆盖的通用自动边。同级采用配置中的第一条。主体显式申请目标时不需要传底层事件。
- `immediate`：允许中途切换。
- `finish_cycle`：当前这一轮完成后切换，适用于循环和非循环动作。
- `complete_transition`：离开时必须有显式 via 路径。已经进入的整段 via 路径均受保护，其他身体动作不能跳过。

拖动位置独立于身体动画。拖动附加姿态通过 `semantics.start_drag` 申请，不能强制切换、跳过受保护过程。

示例（资源名仅示意，需要替换成实际清单中的名称）：

```json
{
  "avatar": { "id": "bird", "description": "围着黄色围巾的小蓝鸟" },
  "defaultState": "nest",
  "states": [
    { "name": "nest", "description": "窝中休息", "loop": true, "interruption": "complete_transition", "clipOptions": [{ "clipName": "rest" }] },
    { "name": "stretch", "description": "伸展身体", "loop": false, "requestable": false, "clipOptions": [{ "clipName": "stand_up" }] },
    { "name": "ready", "description": "精神饱满", "loop": true, "clipOptions": [{ "clipName": "standing" }] }
  ],
  "transactions": [
    { "fromState": "nest", "toState": "ready", "condition": "wake", "via": ["stretch"] },
    { "fromState": "ready", "toState": "nest", "condition": "rest" }
  ],
  "semantics": { "touch.head": "ready", "emotion.joy": "ready" }
}
```

## 语义、策略与请求

`semantics` 把 `emotion.joy/sadness/anger/fear/surprise`、`touch.<region>`、`start_drag/stop_drag/window_sit/window_stand` 等本地意图映射到状态。交互区域由形象声明，大小写须与适配器回传一致；当前 3D 使用 Head、Body、HandL、HandR。没有映射则不编造动画，但触摸事实仍传入主体。

`interactionPolicy.angerRefusalThreshold` 默认 0.85，用户互动时使用主体当前 anger 强度判定是否愿意配合。它只影响动画回应，不能阻止位置拖动，也不取消触摸事实。

`queueLimit` 默认 8，范围 0–32；用户来源优先，模型和调试其次，情绪及自动行为随后。情绪系统已有 expressionQueueLimit 继续限制情绪来源的未完成请求。`requestTimeoutMs` 默认 15000，单个请求有效期范围 1–120000 毫秒。

原有 AI 行为策略中的显式白名单和 forbiddenActions 继续约束模型调用。为形象更换状态名称时，应同步检查 common config 的这些可选策略；空白名单表示由当前形象能力和规则决定。AI 禁用策略不会变成对用户拖动的禁令。

`play_animation` 使用 state；兼容工具 `request_idle_transition` 保留 target_action。返回 queued/started 表示仍未完成；用 request_id 调用 `get_action_transition_status` 查询唯一终态，或使用 `cancel_animation_request`。

completed 表示目标状态的候选实际启动，不能当作表演结束。轮次通过独立事件报告。过期的排队意图立即释放；已进入保护路径的意图过期后报告 expired，仍收尾整条路径，放弃旧目标。主动取消同样等待路径合法结束。后端轮次失联有独立故障看门狗，基于资源时长、播放速率及余量检测。

能力不足时模型可以通过通用气泡输出“（行为描述）”，工具返回的失败不能改写为动画成功。

## 显隐

可通过系统托盘的显示/隐藏操作控制形象。隐藏停止帧定时器、绘制与动画计时，立即取消未完成表现请求，清空队列并保持配置初态，包括受保护路径。显示从初态和零进度开始，不补播旧意图。人格、记忆、情绪和任务继续运行；退出应用与隐藏不同。

表现接口及规则执行器在 `core/presentation`，不依赖 QtGui 或具体渲染 SDK。当前 3D 播放器适配器位于 `core/animation/avatar_3d_backend.*`。替代后端只需提供资源、真实启动/停止、轮次/失败事件、绘制门控及桌面移动。
