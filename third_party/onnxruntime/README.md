# onnxruntime 运行库

本目录存放 onnxruntime C++ 运行库（.dylib/.dll + 头文件），**平台专属、不入库**
（见 `.gitignore`）。新环境/CI 用脚本拉取：

```bash
./tools/fetch_onnxruntime.sh mac        # macOS arm64（开发机）
./tools/fetch_onnxruntime.sh win        # Windows x64（打包交付）
./tools/fetch_onnxruntime.sh all        # 两者
```

Windows PowerShell（不需要 Visual Studio）：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\fetch_onnxruntime.ps1
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

准备好运行库后，用项目现有的 CMake/Ninja/MinGW 工具链重新配置：

```powershell
cmake -S . -B cmake-build-release-mingw_qt -G Ninja `
  -DDESKTOP_PET_ENABLE_ONNX_RUNTIME=ON
cmake --build cmake-build-release-mingw_qt --target memory_strategy_tests
ctest --test-dir cmake-build-release-mingw_qt -R MemoryStrategyTests --output-on-failure
```

Python 侧快速验证使用项目虚拟环境：

```powershell
.\.venv\Scripts\python.exe tools\validate_onnx_windows.py
```

模型文件（bge-small-zh int8 ONNX + vocab）由 `tools/export_bge_onnx.py` 产出，
落在 `assets/embeddings/`，**入库随桌宠分发**。
