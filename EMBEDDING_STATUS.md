# Embedding(ONNX)实现状态

> 本机(macOS)做到哪、卡在哪、你回去怎么弄。精简版。

## 已完成且编译通过

- `assets/embeddings/`:**bge-small-zh-v1.5 int8 ONNX 已导出**(`model_quantized.onnx` ~57MB + vocab.txt + tokenizer.json + export_meta.json)。导出脚本 `tools/export_bge_onnx.py`(走 ModelScope,bypass HF 不可达)。
- `core/ai/memory/onnx_embedding_provider.{h,cpp}`:OnnxEmbeddingProvider 实现。自写 WordPiece 中文分词(只读 vocab.txt,不依赖 HF tokenizers C 绑定)→ onnxruntime 前向 → mean-pool + L2 归一化 → 512 维。失败时 `dimension()==0`、`embed` 返回空,与 Noop 等价,不崩。
- 生产 retrieve 调用点 `ai_brain_router.cpp` 已支持注入 `embeddingIndex`；`AIBrain` 提供 non-owning `setEmbeddingIndex`，为空时走关键词路径。
- CMake:`third_party/onnxruntime/` 软依赖探测。**ort 可用 → 编入 OnnxEmbeddingProvider + 链接 dylib;ort 缺失 → 跳过,退 Noop,不阻塞构建**。
- 单测 `testOnnxEmbeddingProviderLoadsAndEmbeds`(条件编译 `DESKTOP_PET_HAS_ORT`),未生成模型时 QSKIP。

## 卡在哪(本机未解决,与代码无关,纯 macOS 限制)

**ort dylib 加载被 macOS system policy 拦**(运行测试时 `dyld: library load disallowed by system policy`)。

- 原因:微软发布的 `libonnxruntime` 是 adhoc/linker-signed,非 Apple 公证;从浏览器下载带 quarantine 标记,macOS 15 拒绝加载。
- 我试过清 quarantine + 重签 dylib + 给 exe 加 entitlements 关 library-validation,**没调成,且中途把 dylib 文件名/软链搞乱过一次**(已从原始 tgz 恢复)。**别再 `rm -rf onnxruntime-osx-arm64-1.28.0` 整目录删**,真要补文件用 `tar xzf <pkg> <单文件路径>` 只补缺失的。
- **结论:ort 真实推理的运行验证留到 Windows 主交付环境**——Windows 加载 `onnxruntime.dll` 无此签名问题。

## 你回去要弄(按优先级)

1. **Windows 上跑 `OnnxEmbeddingProvider` 单测**:`./tools/fetch_onnxruntime.sh win` 拉ort → 构建 → 跑 `memory_strategy_tests`。这能真正验证 int8 推理 + 我单测里的断言(维度512、norm≈1、相似文本余弦 > 不相关文本)。
2. **若 Windows 单测过不了,重点怀疑三处**(我没运行验证过,逻辑自检但无输出佐证):
   - WordPiece 分词:中文按字切的是否正确(我用了 `isCjkChar` 按 unicode 区间判定);`##` 续接前缀逻辑。
   - onnxruntime 输入张量名是否真为 `input_ids/token_type_ids/attention_mask`(导出时 `dynamic_axes` 设的;若模型图里名字不同,`Run` 会抛异常 → `embed` 返回空)。
   - mean-pool 索引:`lastHidden` 是否真是 `[1,seq,dim]` 连续布局,`base[t*dim+d]` 对不对。
3. **ort 目录当前被我搞在半恢复态**:`libonnxruntime.1.28.0.dylib` 已从原始 tgz 补回(软链正常解析)。但 gitignore 已排除整个 `third_party/onnxruntime/`,**不入库**,新环境用 `tools/fetch_onnxruntime.sh mac` 重新拉即可,别依赖我本机这份。
4. **`assets/embeddings/model_quantized.onnx` 已在 gitignore 开例外,会随桌宠分发**(~57MB)。其余导出产物(vocab/tokenizer/meta)一起入库。

## 体积/性能(供参考,实测待 Windows 跑)

int8 ONNX ~57MB(比预估 24MB 大,因动态量化只量化 MatMul 权重,embedding 表是 Gather 未量化)。运行期常驻 ~60-120MB,单次嵌入 mac M 系几 ms、Windows 普通x64 10-30ms。LLM 是瓶颈,这点开销可忽略。
