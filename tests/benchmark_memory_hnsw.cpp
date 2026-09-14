#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numeric>
#include <random>
#include <vector>

#include "memory/embedding_provider.h"
#include "memory/hnsw_embedding_index.h"
#include "memory/memory_store.h"

namespace {

class BenchmarkEmbeddingProvider : public EmbeddingProvider {
public:
    explicit BenchmarkEmbeddingProvider(int dimension) : m_dimension(dimension) {}
    QString modelName() const override { return QStringLiteral("synthetic-benchmark-%1d").arg(m_dimension); }
    int dimension() const override { return m_dimension; }
    QVector<float> embed(const QString&) override { return {}; }
private:
    int m_dimension = 0;
};

int envInt(const char* name, int fallback, int minimum) {
    bool ok = false;
    const int value = qEnvironmentVariableIntValue(name, &ok);
    return ok ? std::max(value, minimum) : fallback;
}

QVector<float> randomUnitVector(std::mt19937& rng, int dimension) {
    std::normal_distribution<float> normal(0.0f, 1.0f);
    QVector<float> vector(dimension);
    double norm = 0.0;
    for (float& value : vector) {
        value = normal(rng);
        norm += static_cast<double>(value) * value;
    }
    const float invNorm = static_cast<float>(1.0 / std::sqrt(norm));
    for (float& value : vector) value *= invNorm;
    return vector;
}

QVector<float> noisyUnitVector(const QVector<float>& base, std::mt19937& rng, float noise) {
    std::normal_distribution<float> normal(0.0f, noise);
    QVector<float> vector = base;
    double norm = 0.0;
    for (float& value : vector) {
        value += normal(rng);
        norm += static_cast<double>(value) * value;
    }
    const float invNorm = static_cast<float>(1.0 / std::sqrt(norm));
    for (float& value : vector) value *= invNorm;
    return vector;
}

double dot(const QVector<float>& a, const QVector<float>& b) {
    double score = 0.0;
    for (int i = 0; i < a.size(); ++i) score += static_cast<double>(a[i]) * b[i];
    return score;
}

QVector<int> exactTopK(const QVector<QVector<float>>& corpus, const QVector<float>& query, int k) {
    std::vector<std::pair<double, int>> scored;
    scored.reserve(static_cast<size_t>(corpus.size()));
    for (int i = 0; i < corpus.size(); ++i) scored.emplace_back(dot(corpus[i], query), i);
    const int boundedK = std::min(k, static_cast<int>(scored.size()));
    std::nth_element(scored.begin(), scored.begin() + boundedK, scored.end(),
                     [](const auto& lhs, const auto& rhs) { return lhs.first > rhs.first; });
    scored.resize(static_cast<size_t>(boundedK));
    std::sort(scored.begin(), scored.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.first > rhs.first; });
    QVector<int> ids;
    ids.reserve(boundedK);
    for (const auto& item : scored) ids.append(item.second);
    return ids;
}

double recallAtK(const QVector<int>& exact, const QList<EmbeddingSearchResult>& approx) {
    QSet<QString> exactIds;
    for (int id : exact) exactIds.insert(QStringLiteral("mem-%1").arg(id));
    int hits = 0;
    for (const auto& result : approx) {
        if (exactIds.contains(result.memoryId)) ++hits;
    }
    return exact.isEmpty() ? 0.0 : static_cast<double>(hits) / exact.size();
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    const int count = envInt("DESKTOP_PET_HNSW_BENCH_COUNT", 10000, 100);
    const int dimension = envInt("DESKTOP_PET_HNSW_BENCH_DIM", 512, 8);
    const int queries = envInt("DESKTOP_PET_HNSW_BENCH_QUERIES", 100, 1);
    const int topK = envInt("DESKTOP_PET_HNSW_BENCH_TOPK", 32, 1);
    const quint32 seed = static_cast<quint32>(envInt("DESKTOP_PET_HNSW_BENCH_SEED", 20260914, 1));

    QTemporaryDir dir;
    if (!dir.isValid()) return 2;
    MemoryStore store;
    store.setStoragePath(dir.filePath(QStringLiteral("memory.json")));
    store.setDatabasePath(dir.filePath(QStringLiteral("memory.db")));
    if (!store.load()) return 3;

    BenchmarkEmbeddingProvider provider(dimension);
    HnswIndexParams params;
    params.m = envInt("DESKTOP_PET_HNSW_BENCH_M", 16, 1);
    params.efConstruction = envInt("DESKTOP_PET_HNSW_BENCH_EF_CONSTRUCTION", 200, 1);
    params.efSearch = envInt("DESKTOP_PET_HNSW_BENCH_EF_SEARCH", 50, 1);
    params.initialCapacity = count;
    HnswEmbeddingIndex index(store.databaseConnectionName(), &provider, dir.path(), params);

    std::mt19937 rng(seed);
    QVector<QVector<float>> corpus;
    corpus.reserve(count);

    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < count; ++i) {
        QVector<float> vector = randomUnitVector(rng, dimension);
        corpus.append(vector);
        if (!index.upsertVector(QStringLiteral("mem-%1").arg(i), vector,
                                HnswEmbeddingIndex::contentHash(QString::number(i)))) {
            return 4;
        }
    }
    const qint64 buildMs = timer.elapsed();

    std::uniform_int_distribution<int> pick(0, count - 1);
    QVector<QVector<float>> queryVectors;
    queryVectors.reserve(queries);
    for (int i = 0; i < queries; ++i) queryVectors.append(noisyUnitVector(corpus[pick(rng)], rng, 0.01f));

    QVector<double> recalls;
    recalls.reserve(queries);
    QVector<qint64> exactMicros;
    QVector<qint64> hnswMicros;
    exactMicros.reserve(queries);
    hnswMicros.reserve(queries);

    for (const QVector<float>& query : queryVectors) {
        timer.restart();
        const QVector<int> exact = exactTopK(corpus, query, topK);
        exactMicros.append(timer.nsecsElapsed() / 1000);

        timer.restart();
        const QList<EmbeddingSearchResult> approx = index.searchVector(query, topK);
        hnswMicros.append(timer.nsecsElapsed() / 1000);
        recalls.append(recallAtK(exact, approx));
    }

    auto mean = [](const auto& values) {
        if (values.isEmpty()) return 0.0;
        return std::accumulate(values.begin(), values.end(), 0.0) / values.size();
    };
    auto percentile = [](auto values, double p) {
        if (values.isEmpty()) return 0.0;
        std::sort(values.begin(), values.end());
        const int size = static_cast<int>(values.size());
        const int idx = std::clamp(static_cast<int>(std::ceil(p * size)) - 1, 0, size - 1);
        return static_cast<double>(values[idx]);
    };

    QJsonObject report{
        {QStringLiteral("count"), count},
        {QStringLiteral("dimension"), dimension},
        {QStringLiteral("queries"), queries},
        {QStringLiteral("topK"), topK},
        {QStringLiteral("hnswM"), params.m},
        {QStringLiteral("hnswEfConstruction"), params.efConstruction},
        {QStringLiteral("hnswEfSearch"), params.efSearch},
        {QStringLiteral("seed"), static_cast<int>(seed)},
        {QStringLiteral("buildMs"), static_cast<int>(buildMs)},
        {QStringLiteral("recallAtKMean"), mean(recalls)},
        {QStringLiteral("recallAtKP50"), percentile(recalls, 0.50)},
        {QStringLiteral("recallAtKP95"), percentile(recalls, 0.95)},
        {QStringLiteral("exactQueryMicrosMean"), mean(exactMicros)},
        {QStringLiteral("exactQueryMicrosP95"), percentile(exactMicros, 0.95)},
        {QStringLiteral("hnswQueryMicrosMean"), mean(hnswMicros)},
        {QStringLiteral("hnswQueryMicrosP95"), percentile(hnswMicros, 0.95)},
        {QStringLiteral("speedupMean"), mean(hnswMicros) > 0.0 ? mean(exactMicros) / mean(hnswMicros) : 0.0}
    };
    std::cout << QJsonDocument(report).toJson(QJsonDocument::Compact).constData() << std::endl;
    return 0;
}
