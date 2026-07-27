#ifndef DESKTOP_PET_ONNX_EMBEDDING_PROVIDER_H
#define DESKTOP_PET_ONNX_EMBEDDING_PROVIDER_H

#include "embedding_provider.h"

#include <QString>
#include <QVector>
#include <memory>

// ONNX 本地推理的 EmbeddingProvider：加载 bge-small-zh int8 量化 ONNX，
// 文本 → WordPiece tokenize → onnxruntime 前向 → mean-pool + L2 归一化 → 512 维向量。
//
// 设计依据（见 memory/phase1-p0-progress）：
//   - 运行时不联网、不依赖 PyTorch，只链 onnxruntime（third_party 每平台拉取）。
//   - tokenizer 不依赖 HF tokenizers C 绑定，自写 WordPiece 复现 BertTokenizer
//     中文行为（中文按字切、英文/数字走 Whitespace + WordPiece、## 续接前缀），
//     只需 vocab.txt。
//   - 失败（模型缺失/加载失败）时 embed 返回空向量、dimension()==0，与 Noop 等价，
//     检索链路降级不崩；由调用方决定是否回退 NoopEmbeddingProvider。
//
// onnxruntime C++ API 通过 Ort:: 命名空间使用；Env 必须长于 Session 存活，
// 故 Env 置于成员并在 load() 时构造。
class OnnxEmbeddingProvider : public EmbeddingProvider {
public:
    struct Config {
        QString modelPath;      // 如 .../assets/embeddings/model_quantized.onnx
        QString vocabPath;      // 如 .../assets/embeddings/vocab.txt
        int maxSeqLen = 256;     // 运行时截断（config 上限 512，截短省算力）
        int embeddingDim = 512;
        int intraOpThreads = 0;  // 0 = onnxruntime 默认（按核数）
        bool doLowercase = true; // bge-small-zh-v1.5: do_lower_case=true
    };

    OnnxEmbeddingProvider();
    ~OnnxEmbeddingProvider() override;

    // 加载模型与词表。成功后 dimension()==embeddingDim；失败返回 false 并填 errorMessage。
    bool load(const Config& cfg, QString* errorMessage = nullptr);

    // 便捷构造：默认从 assets/embeddings/ 加载，找不到返回 false（不抛异常）。
    static OnnxEmbeddingProvider* tryCreateFromAssets(const QString& assetsDir,
                                                      QString* errorMessage = nullptr);

    QString modelName() const override { return QStringLiteral("bge-small-zh-v1.5"); }
    int dimension() const override { return m_loaded ? m_cfg.embeddingDim : 0; }
    QVector<float> embed(const QString& text) override;

    bool isLoaded() const { return m_loaded; }

private:
    // 前向：input_ids/token_type_ids/attention_mask → last_hidden_state(1,seq,dim)
    bool runForward(const QVector<int64_t>& inputIds,
                    const QVector<int64_t>& tokenTypeIds,
                    const QVector<int64_t>& attentionMask,
                    QVector<float>& lastHidden /* seq*dim */);

    // pImpl 风格：onnxruntime 句柄以不透明成员保存，避免头文件引入大段 ort 头
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    // WordPiece tokenizer 实现（内置）
    class Tokenizer;
    std::unique_ptr<Tokenizer> m_tok;

    Config m_cfg;
    bool m_loaded = false;
};

#endif // DESKTOP_PET_ONNX_EMBEDDING_PROVIDER_H