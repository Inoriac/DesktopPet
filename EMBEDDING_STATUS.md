# Embedding(ONNX)实现状态

> 当前交付环境：Windows / Qt 6.5.3 / MinGW / ONNX Runtime 1.28.0。

## 已完成且编译通过

- `assets/embeddings/`:**bge-small-zh-v1.5 int8 ONNX 已导出**(`model_quantized.onnx` ~57MB + vocab.txt + tokenizer.json + export_meta.json)。导出脚本 `tools/export_bge_onnx.py`(走 ModelScope,bypass HF 不可达)。
- `core/ai/memory/onnx_embedding_provider.{h,cpp}`:OnnxEmbeddingProvider 实现。自写 WordPiece 中文分词(只读 vocab.txt,不依赖 HF tokenizers C 绑定)→ onnxruntime 前向 → mean-pool + L2 归一化 → 512 维。失败时 `dimension()==0`、`embed` 返回空,与 Noop 等价,不崩。
- 生产聊天入口 `ChatPreparationExecutor::Worker` 拥有 ONNX provider、HNSW 索引和独立 SQLite 连接，向图谱/ACT-R 召回传入实际索引。模型加载、推理、恢复和索引维护均在后台 Worker，初始化不阻塞 GUI；缺少 ORT 时保留关键词/激活/图谱召回。
- CMake:`third_party/onnxruntime/` 软依赖探测。**ort 可用 → 编入 OnnxEmbeddingProvider + 链接 dylib;ort 缺失 → 跳过,退 Noop,不阻塞构建**。
- 单测 `testOnnxEmbeddingProviderLoadsAndEmbeds`(条件编译 `DESKTOP_PET_HAS_ORT`),未生成模型时 QSKIP。

## 卡在哪(本机未解决,与代码无关,纯 macOS 限制)

**ort dylib 加载被 macOS system policy 拦**(运行测试时 `dyld: library load disallowed by system policy`)；Windows 验证已完成。

- 原因:微软发布的 `libonnxruntime` 是 adhoc/linker-signed,非 Apple 公证;从浏览器下载带 quarantine 标记,macOS 15 拒绝加载。
- 我试过清 quarantine + 重签 dylib + 给 exe 加 entitlements 关 library-validation,**没调成,且中途把 dylib 文件名/软链搞乱过一次**(已从原始 tgz 恢复)。**别再 `rm -rf onnxruntime-osx-arm64-1.28.0` 整目录删**,真要补文件用 `tar xzf <pkg> <单文件路径>` 只补缺失的。
- Windows 主交付环境已使用官方 `onnxruntime-win-x64-1.28.0` SDK 完成真实推理验证：CMake 检测并链接 ORT，构建后自动部署 `onnxruntime.dll`。
- `MemoryStrategyTests::testOnnxEmbeddingProviderLoadsAndEmbeds` 已通过，确认模型加载、512 维输出、L2 归一化及相似度阈值。
- `ChatPreparationExecutorTests::nativeOnnxRecallRunsThroughChatWorker` 已通过：使用真实模型，召回被 300 条新记录挤出近期窗口的旧偏好，并验证不同措辞的查询。
- Worker 集成测试另覆盖模型构造/使用/销毁的线程归属、慢模型初始化时 GUI 可响应，以及陈旧索引命中已删除/敏感记录时的过滤。

## Windows 复验命令（可选）

1. 重新配置/构建后运行 `ctest --test-dir cmake-build-release-mingw_qt -R "MemoryStrategyTests|ChatPreparationExecutorTests" --output-on-failure`。
2. Python 图验证仍可用 `\.venv\Scripts\python.exe tools\validate_onnx_windows.py`；该脚本刻意不调用会触发部分 ONNX Python 构建原生崩溃的 `onnx.checker`。
3. **若 Windows 单测过不了,重点怀疑三处**:
   - WordPiece 分词:中文按字切的是否正确(我用了 `isCjkChar` 按 unicode 区间判定);`##` 续接前缀逻辑。
   - onnxruntime 输入张量名是否真为 `input_ids/token_type_ids/attention_mask`(导出时 `dynamic_axes` 设的;若模型图里名字不同,`Run` 会抛异常 → `embed` 返回空)。
   - mean-pool 索引:`lastHidden` 是否真是 `[1,seq,dim]` 连续布局,`base[t*dim+d]` 对不对。
4. Windows SDK 当前平铺于 `third_party/onnxruntime/include` 与 `third_party/onnxruntime/lib`，已验证可加载。整个 SDK 目录被 gitignore 排除，新环境需另行准备对应平台 SDK；macOS 历史限制见上文。
5. **`assets/embeddings/model_quantized.onnx` 已在 gitignore 开例外,会随桌宠分发**(~57MB)。其余导出产物(vocab/tokenizer/meta)一起入库。

## 体积/性能(供参考)

int8 ONNX ~57MB（动态量化主要覆盖 MatMul 权重，embedding 表的 Gather 未量化）。历史估算为常驻 ~60-120MB、单次嵌入 ~10-30ms，尚不能视为本机产品性能验收结果。维护与召回在同一 Worker 串行执行，大规模索引重建仍可能延迟聊天准备，但不会在 GUI 线程执行模型或索引工作。
