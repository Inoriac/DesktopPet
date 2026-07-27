# onnxruntime 运行库

本目录存放 onnxruntime C++ 运行库（.dylib/.dll + 头文件），**平台专属、不入库**
（见 `.gitignore`）。新环境/CI 用脚本拉取：

```bash
./tools/fetch_onnxruntime.sh mac        # macOS arm64（开发机）
./tools/fetch_onnxruntime.sh win        # Windows x64（打包交付）
./tools/fetch_onnxruntime.sh all        # 两者
```

脚本优先走 ghproxy 镜像转发（GitHub releases 直链国内限速），失败回退直连。
拉取后目录结构：

```
third_party/onnxruntime/onnxruntime-osx-arm64-1.28.0/
  include/onnxruntime_cxx_api.h
  lib/libonnxruntime.1.28.0.dylib
```

CMake 自动探测此目录；找不到时 `OnnxEmbeddingProvider` 不编入（退化为 Noop，
语义检索链路其余部分照常工作），不阻塞构建。

模型文件（bge-small-zh int8 ONNX + vocab）由 `tools/export_bge_onnx.py` 产出，
落在 `assets/embeddings/`，**入库随桌宠分发**。