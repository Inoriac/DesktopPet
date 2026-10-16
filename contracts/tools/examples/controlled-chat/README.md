# 受控聊天插件

这个示例用独立 SQLite 日志模拟两个聊天会话，供插件作者验证查询、发送、回执与可选监听接入。它不连接网络，也不登录 QQ。主体的 runtime 数据库与本示例的消息日志必须使用不同文件。

| 操作 | 作用 |
| --- | --- |
| list | 列出日志中的会话 |
| read | 按 chat_id、可选 cursor 和 limit 分页读取；limit 为 1—256 |
| send | 实际写入一条自身消息，返回远端 ID、正文和时间 |
| receipt | 按 message_id 查询已存在的消息 |
| source | 可选 JSONL 消息流，与 read 共用事件与消息 ID |
| inject | 仅供本地演示注入其他人的消息，未作为主体 Tool 操作公开 |

## 直接运行适配器

需要 Python 3。以下命令在仓库根目录执行，所有可变数据写入新建临时目录：

```sh
chat_demo_dir="$(mktemp -d)"
chat_demo_state="$chat_demo_dir/chat.sqlite"
python3 contracts/tools/examples/controlled-chat/adapter.py \
  --database "$chat_demo_state" --operation inject <<'JSON'
{"chat_id":"group-a","speaker_id":"alice","text":"第一章看完了，你觉得怎么样？"}
JSON
python3 contracts/tools/examples/controlled-chat/adapter.py \
  --database "$chat_demo_state" --operation read <<'JSON'
{"chat_id":"group-a","limit":64}
JSON
```

inject 返回实际生成的 remote_message_id、content 和 occurred_at；read 返回 items。第二个人使用不同 speaker_id，同一人换昵称仍保留原 ID。查询下一页时传入上一页最后一项的 cursor。

演示发送并查询实际回执：

```sh
python3 contracts/tools/examples/controlled-chat/adapter.py \
  --database "$chat_demo_state" --operation send <<'JSON'
{"chat_id":"group-a","text":"我刚开始读第二章。","reply_to":"message-1"}
JSON
python3 contracts/tools/examples/controlled-chat/adapter.py \
  --database "$chat_demo_state" --operation receipt <<'JSON'
{"message_id":"message-2"}
JSON
```

以上 message-1/message-2 适用于刚创建并按上述顺序操作的日志。重复演示时使用命令实际返回的 ID；receipt 未找到不代表先前发送一定没有发生。

## 生成宿主配置

[make_host_config.py](./make_host_config.py) 输出完整 plugins.json，自动计算宿主稳定会话 ID。将示例 profile UUID 替换为当前角色已有的 profileId，不能使用角色显示名称：

```sh
python3 contracts/tools/examples/controlled-chat/make_host_config.py \
  --profile '替换为角色已有的稳定profileId' \
  --database "$chat_demo_state" \
  > "$chat_demo_dir/plugins.json"
```

脚本默认使用当前 Python 的绝对路径，也可通过 --python 指定解释器。输出文件包含插件根目录、group-a/group-b 的观察与回复绑定、查询结果映射，以及内部自主查询和分会话操作授权。source 默认关闭。

检查生成文件后，将它的字段合并到对应角色 runtime 数据库同目录的 plugins.json，保留角色已安装的其他插件；也可由宿主调用 PluginRuntime::setHostConfiguration 应用。文件直接更新后，重新加载对应角色使配置生效。不要用演示配置覆盖其他角色的设置。

开启 aiSettings.autonomy.enabled 后，主体可在空闲和预算允许时主动选择查看；外部消息不会被默认解释为主人输入。自主授权与内部记忆访问配置相互独立。

## 主动查看与监听

默认配置可以主动查询，无需 source 常驻。仅需查询的宿主配置可用 --without-source 生成；纯查询插件也可以从 plugin.json 省略 sources。

需要监听时，将该 observationBinding.source.enabled 设为 true 后由宿主重新应用配置，或调用 setObservationSource 并使用当前 catalogRevision。关闭 source 不会移除主动查询能力。消息接收和自主开关分开：关闭自主活动时仍可以归档消息。

在另一终端执行 inject 可模拟后续回复。source 与查询共享日志，重复取得同一消息只归档一次。只查询 group-a 不会推进整个账号的监听游标，随后启用 source 仍会收到 group-b 未读取的消息。

可变日志与生成的配置应放在插件目录外。修改插件内容后旧目录摘要失效，需要重新加载配置。

## 验证范围

仓库的 ExternalObservationTests 验证真实适配器/来源进程、重复与跨群消息、事务回滚、游标、回执及生命周期。AutonomousChatTests 通过真实 AIBrain、ModelRouter 的受控 provider、活动 runner、command/source 进程和数据库，验证查看→发送→等待→阅读→收到回复→检查点→恢复，并更换包名与操作名验证相同路径。

这验证了工具契约和主体接线，未验证真实 QQ 登录或真实模型在开放聊天中的行为。开发实际后端时，请继续阅读[观察源与聊天插件接入](../../观察源与聊天插件接入.md)及[插件运行时接入](../../插件运行时接入.md)。
