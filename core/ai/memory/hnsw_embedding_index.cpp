#include "hnsw_embedding_index.h"

#include <hnswlib/hnswlib.h>
#include <hnswlib/space_ip.h>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
QString nowUtc() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODate); }
QByteArray vectorBytes(const QVector<float>& v) {
    return QByteArray(reinterpret_cast<const char*>(v.constData()),
                      v.size() * static_cast<int>(sizeof(float)));
}
}

HnswEmbeddingIndex::HnswEmbeddingIndex(QString connectionName,
                                       EmbeddingProvider* provider,
                                       QString indexDirectory,
                                       HnswIndexParams params)
    : m_connectionName(std::move(connectionName)),
      m_provider(provider),
      m_indexDirectory(std::move(indexDirectory)),
      m_params(params) {}

HnswEmbeddingIndex::~HnswEmbeddingIndex() = default;

QString HnswEmbeddingIndex::contentHash(const QString& text) {
    return QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha1).toHex());
}

void HnswEmbeddingIndex::normalize(QVector<float>& v) {
    double norm = 0.0;
    for (float x : v) norm += static_cast<double>(x) * x;
    norm = std::sqrt(norm);
    if (norm < 1e-12) return;
    for (float& x : v) x = static_cast<float>(x / norm);
}

QString HnswEmbeddingIndex::modelFileToken() const {
    const QString model = m_provider ? m_provider->modelName() : QStringLiteral("none");
    return contentHash(model).left(8);
}

QString HnswEmbeddingIndex::indexFilePath() const {
    return QDir(m_indexDirectory).filePath(QStringLiteral("memory_hnsw_%1.bin").arg(modelFileToken()));
}
QString HnswEmbeddingIndex::metaFilePath() const { return indexFilePath() + QStringLiteral(".meta.json"); }

bool HnswEmbeddingIndex::ensureIndexAllocated(int dimension) {
    if (dimension <= 0) return false;
    if (m_index && m_dimension == dimension) return true;
    m_space = std::make_unique<hnswlib::InnerProductSpace>(dimension);
    m_index = std::make_unique<hnswlib::HierarchicalNSW<float>>(
        m_space.get(), static_cast<size_t>(std::max(16, m_params.initialCapacity)),
        m_params.m, m_params.efConstruction);
    m_index->setEf(m_params.efSearch);
    m_dimension = dimension;
    m_tombstones = 0;
    return true;
}

qint64 HnswEmbeddingIndex::allocateLabel(const QString& memoryId, const QString& hash) {
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) return -1;
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT label FROM memory_hnsw_labels WHERE memory_id=:id AND model=:model"));
    q.bindValue(QStringLiteral(":id"), memoryId);
    q.bindValue(QStringLiteral(":model"), m_provider->modelName());
    if (q.exec() && q.next()) return q.value(0).toLongLong();
    QSqlQuery insert(db);
    insert.prepare(QStringLiteral("INSERT INTO memory_hnsw_labels(memory_id,model,label,content_hash,status,created_at) "
                                  "VALUES(:id,:model,(SELECT COALESCE(MAX(label),-1)+1 FROM memory_hnsw_labels WHERE model=:model2),:hash,'Active',:ts)"));
    insert.bindValue(QStringLiteral(":id"), memoryId);
    insert.bindValue(QStringLiteral(":model"), m_provider->modelName());
    insert.bindValue(QStringLiteral(":model2"), m_provider->modelName());
    insert.bindValue(QStringLiteral(":hash"), hash);
    insert.bindValue(QStringLiteral(":ts"), nowUtc());
    if (!insert.exec()) return -1;
    QSqlQuery read(db);
    read.prepare(QStringLiteral("SELECT label FROM memory_hnsw_labels WHERE memory_id=:id AND model=:model"));
    read.bindValue(QStringLiteral(":id"), memoryId);
    read.bindValue(QStringLiteral(":model"), m_provider->modelName());
    return read.exec() && read.next() ? read.value(0).toLongLong() : -1;
}

bool HnswEmbeddingIndex::updateLabelStatus(qint64 label, const QString& status) {
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE memory_hnsw_labels SET status=:status,updated_at=:ts WHERE model=:model AND label=:label"));
    q.bindValue(QStringLiteral(":status"), status);
    q.bindValue(QStringLiteral(":ts"), nowUtc());
    q.bindValue(QStringLiteral(":model"), m_provider->modelName());
    q.bindValue(QStringLiteral(":label"), label);
    return q.exec();
}

bool HnswEmbeddingIndex::insertVectorLocked(const QString& memoryId, QVector<float>& vector, const QString& hash) {
    normalize(vector);
    if (vector.isEmpty() || !ensureIndexAllocated(vector.size())) return false;
    auto old = m_activeLabelByMemory.find(memoryId);
    if (old != m_activeLabelByMemory.end()) {
        if (m_hashByMemory.value(memoryId) == hash) return true;
        m_index->markDelete(old.value());
        updateLabelStatus(old.value(), QStringLiteral("Deleted"));
        ++m_tombstones;
        m_memoryByLabel.remove(old.value());
        m_activeLabelByMemory.erase(old);
    }
    const qint64 label = allocateLabel(memoryId, hash);
    if (label < 0) return false;
    try {
        m_index->addPoint(vector.constData(), static_cast<hnswlib::labeltype>(label));
    } catch (...) { return false; }
    m_activeLabelByMemory.insert(memoryId, label);
    m_memoryByLabel.insert(label, memoryId);
    m_hashByMemory.insert(memoryId, hash);
    updateLabelStatus(label, QStringLiteral("Active"));
    return true;
}

bool HnswEmbeddingIndex::upsertVector(const QString& memoryId, QVector<float> vector, const QString& hash) {
    QWriteLocker locker(&m_lock);
    return insertVectorLocked(memoryId, vector, hash);
}

bool HnswEmbeddingIndex::upsert(const QString& memoryId, const QString& text) {
    if (!m_provider || m_provider->dimension() <= 0) return false;
    const QVector<float> vector = m_provider->embed(text);
    if (vector.isEmpty()) return false;
    return upsertVector(memoryId, vector, contentHash(text));
}

bool HnswEmbeddingIndex::removeVector(const QString& memoryId) {
    QWriteLocker locker(&m_lock);
    auto it = m_activeLabelByMemory.find(memoryId);
    if (it == m_activeLabelByMemory.end()) return true;
    m_index->markDelete(it.value());
    updateLabelStatus(it.value(), QStringLiteral("Deleted"));
    m_memoryByLabel.remove(it.value());
    m_activeLabelByMemory.erase(it);
    m_hashByMemory.remove(memoryId);
    ++m_tombstones;
    return true;
}

bool HnswEmbeddingIndex::remove(const QString& memoryId) { return removeVector(memoryId); }

QList<EmbeddingSearchResult> HnswEmbeddingIndex::searchVector(const QVector<float>& input, int limit) const {
    QList<EmbeddingSearchResult> out;
    if (limit <= 0) return out;
    QVector<float> query = input;
    normalize(query);
    QReadLocker locker(&m_lock);
    if (!m_index || query.size() != m_dimension || query.isEmpty()) return out;
    const size_t count = std::min<size_t>(static_cast<size_t>(limit), m_activeLabelByMemory.size());
    if (!count) return out;
    auto matches = m_index->searchKnn(query.constData(), count);
    while (!matches.empty()) {
        const auto item = matches.top(); matches.pop();
        const QString id = m_memoryByLabel.value(static_cast<qint64>(item.second));
        if (!id.isEmpty()) {
            EmbeddingSearchResult result;
            result.memoryId = id;
            result.similarity = 1.0 - item.first;
            out.prepend(result);
        }
    }
    return out;
}

QList<EmbeddingSearchResult> HnswEmbeddingIndex::search(const QString& query, int limit) {
    if (!m_provider || m_provider->dimension() <= 0 || query.isEmpty()) return {};
    return searchVector(m_provider->embed(query), limit);
}

bool HnswEmbeddingIndex::loadLabelMapsFromDatabase() {
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT memory_id,label,content_hash FROM memory_hnsw_labels WHERE model=:model AND status='Active'"));
    q.bindValue(QStringLiteral(":model"), m_provider->modelName());
    if (!q.exec()) return false;
    m_activeLabelByMemory.clear(); m_memoryByLabel.clear(); m_hashByMemory.clear();
    while (q.next()) {
        const QString id = q.value(0).toString(); const qint64 label = q.value(1).toLongLong();
        m_activeLabelByMemory.insert(id, label); m_memoryByLabel.insert(label, id); m_hashByMemory.insert(id, q.value(2).toString());
    }
    return true;
}

bool HnswEmbeddingIndex::saveToDisk(QString* errorMessage) {
    QReadLocker locker(&m_lock);
    if (!m_index) return false;
    QDir().mkpath(m_indexDirectory);
    const QString tmp = indexFilePath() + QStringLiteral(".tmp");
    try { m_index->saveIndex(tmp.toStdString()); } catch (...) { if (errorMessage) *errorMessage = QStringLiteral("hnsw save failed"); return false; }
    if (!QFile::remove(indexFilePath()) && QFile::exists(indexFilePath())) return false;
    if (!QFile::rename(tmp, indexFilePath())) return false;
    QJsonObject meta{{QStringLiteral("model"), m_provider->modelName()}, {QStringLiteral("dimension"), m_dimension},
                     {QStringLiteral("m"), m_params.m}, {QStringLiteral("efConstruction"), m_params.efConstruction},
                     {QStringLiteral("generation"), ++m_generation}, {QStringLiteral("activeCount"), m_activeLabelByMemory.size()}};
    QFile file(metaFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    file.write(QJsonDocument(meta).toJson(QJsonDocument::Compact));
    return true;
}

bool HnswEmbeddingIndex::loadFromDisk(QString* errorMessage) {
    QWriteLocker locker(&m_lock);
    QFile file(metaFilePath());
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QJsonObject meta = QJsonDocument::fromJson(file.readAll()).object();
    if (meta.value(QStringLiteral("model")).toString() != m_provider->modelName() ||
        meta.value(QStringLiteral("m")).toInt() != m_params.m ||
        meta.value(QStringLiteral("efConstruction")).toInt() != m_params.efConstruction) return false;
    m_dimension = meta.value(QStringLiteral("dimension")).toInt();
    if (!ensureIndexAllocated(m_dimension)) return false;
    try { m_index->loadIndex(indexFilePath().toStdString(), m_space.get()); } catch (...) { if (errorMessage) *errorMessage = QStringLiteral("hnsw load failed"); return false; }
    m_index->setEf(m_params.efSearch);
    m_generation = meta.value(QStringLiteral("generation")).toInteger();
    return loadLabelMapsFromDatabase();
}

bool HnswEmbeddingIndex::rebuildFromRepository(QString* errorMessage) {
    QWriteLocker locker(&m_lock);
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen() || !m_provider || m_provider->dimension() <= 0) return false;
    if (!ensureIndexAllocated(m_provider->dimension())) return false;
    m_index = std::make_unique<hnswlib::HierarchicalNSW<float>>(m_space.get(), static_cast<size_t>(std::max(16, m_params.initialCapacity)), m_params.m, m_params.efConstruction);
    m_index->setEf(m_params.efSearch);
    m_activeLabelByMemory.clear(); m_memoryByLabel.clear(); m_hashByMemory.clear(); m_tombstones = 0;
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT id,summary,content FROM memory_items WHERE status='active' AND privacy_level!='sensitive' AND partition!='hippocampus'"))) return false;
    while (q.next()) {
        const QString id = q.value(0).toString(); const QString text = q.value(1).toString() + QStringLiteral("\n") + q.value(2).toString();
        const QVector<float> vector = m_provider->embed(text);
        if (vector.isEmpty()) continue;
        QVector<float> normalizedVector = vector;
        if (!insertVectorLocked(id, normalizedVector, contentHash(text))) continue;
    }
    locker.unlock();
    return saveToDisk(errorMessage);
}

bool HnswEmbeddingIndex::isReady() const { QReadLocker l(&m_lock); return m_index != nullptr; }
int HnswEmbeddingIndex::activeCount() const { QReadLocker l(&m_lock); return m_activeLabelByMemory.size(); }
int HnswEmbeddingIndex::tombstoneCount() const { QReadLocker l(&m_lock); return m_tombstones; }
double HnswEmbeddingIndex::tombstoneRatio() const { QReadLocker l(&m_lock); const int total = m_activeLabelByMemory.size() + m_tombstones; return total ? static_cast<double>(m_tombstones) / total : 0.0; }
bool HnswEmbeddingIndex::needsCompaction() const { return tombstoneRatio() >= m_params.tombstoneRebuildRatio; }
