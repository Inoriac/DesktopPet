#include "onnx_embedding_provider.h"

#include <onnxruntime_cxx_api.h>

#include <QFile>
#include <QTextStream>
#include <QSet>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace {

// ---- WordPiece tokenizer：复现 transformers BertTokenizer 中文行为 ----
//   - 中文按字切（CJK 字符独立成 token）
//   - 英文/数字：按空白分词后 WordPiece（## 续接前缀）
//   - do_lower_case：小写
//   - 只需 vocab.txt，不依赖 HF tokenizers C 绑定
//
// bge-small-zh-v1.5 词表标准特殊位：[PAD]=0 [UNK]=1 [CLS]=101 [SEP]=102 [MASK]=103，
// 但实现不假定固定 ID，按词表内容查找，找不到则用 [UNK]。

bool isCjkChar(quint32 cp) {
    // CJK Unified Ideographs 及常见扩展，覆盖 BGE 词表的中文按字切判定。
    return (cp >= 0x4E00 && cp <= 0x9FFF) ||
           (cp >= 0x3400 && cp <= 0x4DBF) ||
           (cp >= 0x20000 && cp <= 0x2A6DF) ||
           (cp >= 0x2A700 && cp <= 0x2B73F) ||
           (cp >= 0x2B740 && cp <= 0x2B81F) ||
           (cp >= 0x2B820 && cp <= 0x2CEAF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0x2F800 && cp <= 0x2FA1F);
}

bool isWhitespace(QChar c) {
    const int u = c.unicode();
    if (u == 0x20 || u == 0x09 || u == 0x0A || u == 0x0D) return true;
    return c.category() == QChar::Separator_Space;
}

bool isControl(QChar c) {
    if (c == '\t' || c == '\n' || c == '\r') return false;
    return c.category() == QChar::Other_Control || c.category() == QChar::Other_Format;
}

} // namespace

class OnnxEmbeddingProvider::Tokenizer {
public:
    bool load(const QString& vocabPath, QString* err) {
        QFile f(vocabPath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            if (err) *err = QStringLiteral("cannot open vocab: %1").arg(vocabPath);
            return false;
        }
        QTextStream in(&f);
        in.setEncoding(QStringConverter::Utf8);
        QString line;
        int id = 0;
        while (in.readLineInto(&line)) {
            // vocab.txt 行尾可能带 \r（CRLF），QTextStream 已处理主流情况，再保险一次。
            if (line.endsWith('\r')) line.chop(1);
            if (line.isEmpty()) { id++; continue; }
            vocab_[line.toStdString()] = id;
            id2tok_[id] = line.toStdString();
            id++;
        }
        // 定位特殊 token
        auto lookup = [&](const char* t) -> int {
            auto it = vocab_.find(t);
            return it == vocab_.end() ? -1 : it->second;
        };
        unkId_ = lookup("[UNK]");
        clsId_ = lookup("[CLS]");
        sepId_ = lookup("[SEP]");
        padId_ = lookup("[PAD]");
        if (clsId_ < 0 || sepId_ < 0) {
            if (err) *err = QStringLiteral("vocab missing [CLS]/[SEP]");
            return false;
        }
        return true;
    }

    int seqPaddingLen() const { return id2tok_.size(); }

    // encode → input_ids（已含 [CLS] ... [SEP]），并填充等长的 token_type_ids/attention_mask。
    void encode(const QString& text, int maxSeqLen,
                QVector<int64_t>& inputIds,
                QVector<int64_t>& tokenTypeIds,
                QVector<int64_t>& attentionMask) const {
        std::vector<std::string> tokens;
        // 1) 基础分词：中文按字、其余按空白聚合后 WordPiece
        const std::string unkSub = "##";
        // 先按字符遍历构建 word 列表（中文直接成 word；其余累积，遇空白 flush）
        auto flushWord = [&](const QString& word) {
            if (word.isEmpty()) return;
            // WordPiece 切分当前 word
            wordpiece(word, tokens);
        };

        QString cur;
        for (const QChar& ch : text) {
            if (isControl(ch)) continue;
            if (isWhitespace(ch)) {
                flushWord(cur);
                cur.clear();
                continue;
            }
            QChar c = doLower_ ? ch.toLower() : ch;
            if (isCjkChar(c.unicode())) {
                // 中文：先 flush 当前累积的英文 word，再把该字作为独立 word
                flushWord(cur);
                cur.clear();
                wordpiece(QString(c), tokens);
            } else {
                cur.append(c);
            }
        }
        flushWord(cur);

        // 2) 拼接 [CLS] tokens... [SEP]，截断到 maxSeqLen
        inputIds.clear();
        inputIds.reserve(maxSeqLen);
        inputIds.push_back(clsId_);
        for (const auto& t : tokens) {
            if (static_cast<int>(inputIds.size()) >= maxSeqLen - 1) break; // 留 [SEP]
            auto it = vocab_.find(t);
            inputIds.push_back(it == vocab_.end() ? unkId_ : it->second);
        }
        inputIds.push_back(sepId_);

        const int seq = static_cast<int>(inputIds.size());
        tokenTypeIds = QVector<int64_t>(seq, 0);
        attentionMask = QVector<int64_t>(seq, 1);
    }

private:
    void wordpiece(const QString& word, std::vector<std::string>& out) const {
        const int maxChars = 100;
        const std::string w = word.toStdString();
        if (static_cast<int>(w.size()) > maxChars) {
            out.push_back("[UNK]");
            return;
        }
        // 以字节切片会切坏多字节 UTF-8；改用字符切片。
        // QString -> vector<string>（每个字符一个 UTF-8 子串）
        std::vector<std::string> chars;
        chars.reserve(word.size());
        for (const QChar& ch : word) chars.emplace_back(QString(ch).toStdString());

        int start = 0;
        const int n = static_cast<int>(chars.size());
        while (start < n) {
            int end = n;
            std::string curTok;
            bool found = false;
            while (start < end) {
                std::string sub;
                for (int i = start; i < end; ++i) sub += chars[i];
                std::string key = (start == 0) ? sub : ("##" + sub);
                if (vocab_.find(key) != vocab_.end()) {
                    curTok = key;
                    found = true;
                    break;
                }
                end--;
            }
            if (!found) {
                out.push_back("[UNK]");
                return;
            }
            out.push_back(curTok);
            start = end;
        }
    }

    std::unordered_map<std::string, int> vocab_;
    std::unordered_map<int, std::string> id2tok_;
    int unkId_ = 1, clsId_ = 101, sepId_ = 102, padId_ = 0;
    bool doLower_ = true;
};

// ---- onnxruntime 句柄封装（避免头文件暴露 ort 头） ----
struct OnnxEmbeddingProvider::Impl {
    Ort::Env env{nullptr};
    Ort::Session session{nullptr};
    Ort::AllocatorWithDefaultOptions allocator;
};

OnnxEmbeddingProvider::OnnxEmbeddingProvider()
    : m_impl(std::make_unique<Impl>())
    , m_tok(std::make_unique<Tokenizer>())
{
}

OnnxEmbeddingProvider::~OnnxEmbeddingProvider() = default;

bool OnnxEmbeddingProvider::load(const Config& cfg, QString* errorMessage)
{
    m_cfg = cfg;

    // 1) 词表
    QString terr;
    if (!m_tok->load(cfg.vocabPath, &terr)) {
        if (errorMessage) *errorMessage = terr;
        m_loaded = false;
        return false;
    }

    // 2) ONNX session
    try {
        m_impl->env = Ort::Env(ORT_LOGGING_LEVEL_WARNING, "desktop_pet_bge");
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(cfg.intraOpThreads > 0 ? cfg.intraOpThreads : 1);
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        // 路径转 UTF-8
        const QByteArray mp = cfg.modelPath.toUtf8();
        m_impl->session = Ort::Session(m_impl->env, mp.constData(), opts);
    } catch (const Ort::Exception& e) {
        if (errorMessage) *errorMessage = QStringLiteral("onnx session: %1").arg(e.what());
        m_loaded = false;
        return false;
    } catch (const std::exception& e) {
        if (errorMessage) *errorMessage = QStringLiteral("onnx session: %1").arg(e.what());
        m_loaded = false;
        return false;
    }
    m_loaded = true;
    return true;
}

OnnxEmbeddingProvider* OnnxEmbeddingProvider::tryCreateFromAssets(const QString& assetsDir,
                                                                  QString* errorMessage)
{
    auto* p = new OnnxEmbeddingProvider();
    Config cfg;
    cfg.modelPath = assetsDir + QStringLiteral("/embeddings/model_quantized.onnx");
    cfg.vocabPath = assetsDir + QStringLiteral("/embeddings/vocab.txt");
    if (!p->load(cfg, errorMessage)) {
        delete p;
        return nullptr;
    }
    return p;
}

bool OnnxEmbeddingProvider::runForward(const QVector<int64_t>& inputIds,
                                       const QVector<int64_t>& tokenTypeIds,
                                       const QVector<int64_t>& attentionMask,
                                       QVector<float>& lastHidden)
{
    const int64_t seq = static_cast<int64_t>(inputIds.size());
    const int64_t dim = static_cast<int64_t>(m_cfg.embeddingDim);
    const int64_t shape[2] = {1, seq};

    try {
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value inIds = Ort::Value::CreateTensor<int64_t>(mem, const_cast<int64_t*>(inputIds.data()), seq, shape, 2);
        Ort::Value inTypes = Ort::Value::CreateTensor<int64_t>(mem, const_cast<int64_t*>(tokenTypeIds.data()), seq, shape, 2);
        Ort::Value inMask = Ort::Value::CreateTensor<int64_t>(mem, const_cast<int64_t*>(attentionMask.data()), seq, shape, 2);

        const char* inNames[3] = {"input_ids", "token_type_ids", "attention_mask"};
        const char* outNames[1] = {"last_hidden_state"};
        Ort::Value inputs[3] = {std::move(inIds), std::move(inTypes), std::move(inMask)};

        auto outputs = m_impl->session.Run(Ort::RunOptions{nullptr}, inNames, inputs, 3, outNames, 1);
        if (outputs.empty()) return false;

        auto& out = outputs.front();
        auto info = out.GetTensorTypeAndShapeInfo();
        auto outShape = info.GetShape(); // [1, seq, dim]
        const size_t total = static_cast<size_t>(outShape[0]) * outShape[1] * outShape[2];
        const float* data = out.GetTensorData<float>();
        lastHidden.resize(static_cast<int>(total));
        std::memcpy(lastHidden.data(), data, total * sizeof(float));
        return true;
    } catch (const Ort::Exception& e) {
        Q_UNUSED(e)
        return false;
    } catch (const std::exception&) {
        return false;
    }
}

QVector<float> OnnxEmbeddingProvider::embed(const QString& text)
{
    if (!m_loaded) return {};

    QVector<int64_t> inputIds, tokenTypeIds, attentionMask;
    m_tok->encode(text, m_cfg.maxSeqLen, inputIds, tokenTypeIds, attentionMask);
    if (inputIds.size() < 2) return QVector<float>(m_cfg.embeddingDim, 0.0f);

    QVector<float> lastHidden;
    if (!runForward(inputIds, tokenTypeIds, attentionMask, lastHidden)) return {};

    const int seq = static_cast<int>(inputIds.size());
    const int dim = m_cfg.embeddingDim;

    // lastHidden 布局若被模型展平成 [1, seq, dim] 连续；按 seq 行 × dim 列取 mean pool。
    // 跳过 [CLS]/[SEP] 不参与?BGE 聚类用 mean of *all* token embeddings（含特殊符），与
    // sentence-transformers 默认一致。这里全 token（attention_mask 全 1）取均值。
    QVector<float> pooled(dim, 0.0f);
    const float* base = lastHidden.constData();
    // 若 lastHidden.size()==seq*dim 则按预期；否则尽力按 seq 处理
    if (lastHidden.size() >= seq * dim) {
        for (int d = 0; d < dim; ++d) {
            double s = 0.0;
            for (int t = 0; t < seq; ++t) s += base[t * dim + d];
            pooled[d] = static_cast<float>(s / seq);
        }
    }

    // L2 归一化
    double norm = 0.0;
    for (float v : pooled) norm += double(v) * double(v);
    norm = std::sqrt(norm);
    if (norm > 0.0) {
        const float inv = float(1.0 / norm);
        for (float& v : pooled) v *= inv;
    }
    return pooled;
}