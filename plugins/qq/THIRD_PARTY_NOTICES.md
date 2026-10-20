# API 与第三方来源

本插件是面向 DesktopPet 通用工具/观察契约的原创适配实现，不嵌入 MaiBot SDK，也没有复制下述 GPL 项目的源码。借鉴的是后台连接、消息转换、账号校验、会话隔离和发送确认的设计。项目自身许可见[LICENSE.md](../../LICENSE.md)。

## 研究参考

- [MaiBot](https://github.com/Mai-with-u/MaiBot/tree/ff0fe42500f5f7581e9655d1e78379407233207a)，固定提交 `ff0fe42500f5f7581e9655d1e78379407233207a`。研究平台路由、消息到达与回复选择分离；项目声明 GPL-3.0。
- [MaiBot-SnowLuma-Adapter](https://github.com/Mai-with-u/MaiBot-SnowLuma-Adapter/tree/a93a0ad1e27e75e699a4e10571a2cda2884a81cd)，固定提交 `a93a0ad1e27e75e699a4e10571a2cda2884a81cd`，1.1.7，声明 GPL-v3.0-or-later。研究 NapCat/SnowLuma 的正向 WS、echo 相关、send_msg 差异与 SnowLuma message_seq 确认；没有复用其 MaiBot 插件接口。
- [OneBot v11 HTTP/WebSocket API](https://github.com/botuniverse/onebot-11/tree/master/api/public.md)：get_login_info、get_msg、send_private_msg、send_group_msg、send_msg、消息段与事件封装的协议依据。
- [NapCat](https://github.com/NapNeko/NapCatQQ/tree/450fb0429a064e3418e0c698b1b4d78324101929)，固定提交 `450fb0429a064e3418e0c698b1b4d78324101929`：核对 `get_msg` 私聊 sender/user_id 与缺少接收方字段的实际构造。

get_group_msg_history / get_friend_msg_history、message_seq 以及具体后端兼容性不是统一保证的 OneBot 核心能力；部署版本需按[验收交接](验收交接.md)记录。未支持历史读取时只返回本地缓存并标记 local_only；发送证据不足保持 unknown。

## 唯一运行依赖

`websockets==15.0.1`，用于 Python WebSocket 客户端；测试假服务器也使用它。安装在使用者指定的包外 venv。本包不包含该库的源码副本。

来源：[python-websockets/websockets 15.0.1](https://github.com/python-websockets/websockets/tree/15.0.1)，BSD-3-Clause。安装包保留其许可证；如果再分发依赖，请同时保留下列原文：

```text
Copyright (c) Aymeric Augustin and contributors

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the names of its contributors
      may be used to endorse or promote products derived from this software
      without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
