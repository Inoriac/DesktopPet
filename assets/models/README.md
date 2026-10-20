# 桌宠模型资产

`assets/models/` 下的角色模型体积较大，并受各自授权条款约束，因此不随 Git 仓库分发。请只放入自己有权使用的资产，不要提交模型、纹理、动画或由这些文件生成的缓存。

launcher 首次创建的默认角色为 `Milltina`，默认查找：

```text
assets/models/milltina/model/milltina.gltf
```

该 GLTF 引用的纹理、二进制缓冲区和动画文件必须保持相对路径可解析。放置完成后从仓库根目录启动 launcher；核心会把仓库根目录作为资源根目录。

使用其他合法模型时，修改本机角色清单 `%APPDATA%\Desktop Pet Team\Desktop Pet\pets.json` 中对应角色的 `modelPath`。路径可以相对仓库根目录，也可以使用绝对路径。保留既有 `profileId`，它是该角色记忆、人物关系、会话和插件数据的稳定身份；更换显示名或模型时不要随意生成新的 UUID。

如果窗口和托盘均正常但形象为空，先检查核心日志中的 `Failed to load model`，再核对模型路径、GLTF 外部文件、文件权限和 OpenGL 3.3 驱动。
