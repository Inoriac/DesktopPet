#include "hnsw_embedding_index.h"

#include <hnswlib/hnswlib.h>
#include <hnswlib/space_ip.h>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QUuid>
#include <QSqlQuery>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
QString nowUtc() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODate); }
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

bool HnswEmbeddingIndex::normalize(QVector<float>& v) {
    double norm = 0.0;
    for (float x : v) norm += static_cast<double>(x) * x;
    if (!std::isfinite(norm) || norm <= 1e-12) return false;
    norm = std::sqrt(norm);
    for (float& x : v) x = static_cast<float>(x / norm);
    return true;
}

void HnswEmbeddingIndex::resetLocked() {
    m_index.reset();
    m_space.reset();
    m_dimension = 0;
    m_activeLabelByMemory.clear();
    m_memoryByLabel.clear();
    m_hashByMemory.clear();
    m_tombstones = 0;
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
    if (m_index) return m_dimension == dimension;
    if (m_params.m <= 0 || m_params.efConstruction <= 0 || m_params.efSearch <= 0) return false;
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
    q.prepare(QStringLiteral("SELECT label,content_hash FROM memory_hnsw_labels WHERE memory_id=:id AND model=:model"));
    q.bindValue(QStringLiteral(":id"), memoryId);
    q.bindValue(QStringLiteral(":model"), m_provider->modelName());
    if (!q.exec()) return -1;
    if (q.next()) {
        if (q.value(1).toString() == hash) return q.value(0).toLongLong();
        QSqlQuery next(db);
        next.prepare(QStringLiteral("SELECT COALESCE(MAX(label),-1)+1 FROM memory_hnsw_labels WHERE model=:model"));
        next.bindValue(QStringLiteral(":model"), m_provider->modelName());
        if (!next.exec() || !next.next()) return -1;
        const qint64 label = next.value(0).toLongLong();
        QSqlQuery update(db);
        update.prepare(QStringLiteral("UPDATE memory_hnsw_labels SET label=:label,content_hash=:hash,status='Active',updated_at=:ts WHERE memory_id=:id AND model=:model"));
        update.bindValue(QStringLiteral(":label"), label);
        update.bindValue(QStringLiteral(":hash"), hash);
        update.bindValue(QStringLiteral(":ts"), nowUtc());
        update.bindValue(QStringLiteral(":id"), memoryId);
        update.bindValue(QStringLiteral(":model"), m_provider->modelName());
        return update.exec() ? label : -1;
    }
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
    return q.exec() && q.numRowsAffected() == 1;
}

bool HnswEmbeddingIndex::insertVectorLocked(const QString& memoryId, QVector<float>& vector, const QString& hash) {
    if (!m_provider || memoryId.isEmpty() || vector.size() != m_provider->dimension() ||
        !normalize(vector) || !ensureIndexAllocated(vector.size())) return false;
    auto old = m_activeLabelByMemory.find(memoryId);
    if (old != m_activeLabelByMemory.end()) {
        if (m_hashByMemory.value(memoryId) == hash) return true;
        if (!updateLabelStatus(old.value(), QStringLiteral("Deleted"))) return false;
        m_index->markDelete(old.value());
        ++m_tombstones;
        m_memoryByLabel.remove(old.value());
        m_activeLabelByMemory.erase(old);
    }
    const qint64 label = allocateLabel(memoryId, hash);
    if (label < 0) return false;
    try {
        if (m_index->getCurrentElementCount() >= m_index->getMaxElements()) {
            m_index->resizeIndex(std::max<size_t>(m_index->getMaxElements() * 2, m_index->getCurrentElementCount() + 1));
        }
        m_index->addPoint(vector.constData(), static_cast<hnswlib::labeltype>(label));
    } catch (...) { return false; }
    if (!updateLabelStatus(label, QStringLiteral("Active"))) {
        resetLocked();
        return false;
    }
    m_activeLabelByMemory.insert(memoryId, label);
    m_memoryByLabel.insert(label, memoryId);
    m_hashByMemory.insert(memoryId, hash);
    m_tombstones = static_cast<int>(m_index->getDeletedCount());
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
    if (!updateLabelStatus(it.value(), QStringLiteral("Deleted"))) return false;
    m_index->markDelete(it.value());
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
    if (!normalize(query)) return out;
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
    QWriteLocker locker(&m_lock);
    return saveToDiskLocked(errorMessage);
}

bool HnswEmbeddingIndex::saveToDiskLocked(QString* errorMessage) {
    if (!m_provider || m_provider->dimension() <= 0) return false;
    if (!m_index && !ensureIndexAllocated(m_provider->dimension())) return false;
    if (!QDir().mkpath(m_indexDirectory)) return false;
    QTemporaryFile temporary(QDir(m_indexDirectory).filePath(QStringLiteral("hnsw-XXXXXX")));
    if (!temporary.open()) return false;
    const QString tmp = temporary.fileName();
    temporary.close();
    try { m_index->saveIndex(tmp.toStdString()); } catch (...) {
        if (errorMessage) *errorMessage = QStringLiteral("hnsw save failed");
        return false;
    }
    QFile source(tmp);
    if (!source.open(QIODevice::ReadOnly) || source.size() != static_cast<qint64>(m_index->indexFileSize()))
        return false;
    QSaveFile binary(indexFilePath());
    if (!binary.open(QIODevice::WriteOnly)) return false;
    QCryptographicHash digest(QCryptographicHash::Sha256);
    while (!source.atEnd()) {
        const QByteArray block = source.read(64 * 1024);
        if (block.isEmpty() || binary.write(block) != block.size()) return false;
        digest.addData(block);
    }
    if (!binary.commit()) return false;
    const qint64 nextGeneration = m_generation + 1;
    QJsonObject meta{{QStringLiteral("model"), m_provider->modelName()}, {QStringLiteral("dimension"), m_dimension},
                     {QStringLiteral("m"), m_params.m}, {QStringLiteral("efConstruction"), m_params.efConstruction},
                     {QStringLiteral("generation"), nextGeneration}, {QStringLiteral("activeCount"), m_activeLabelByMemory.size()},
                     {QStringLiteral("distance"), QStringLiteral("cosine")},
                     {QStringLiteral("contentHashRule"), QStringLiteral("sha1-utf8")},
                     {QStringLiteral("binarySha256"), QString::fromLatin1(digest.result().toHex())}};
    QSaveFile file(metaFilePath());
    if (!file.open(QIODevice::WriteOnly)) return false;
    const QByteArray json = QJsonDocument(meta).toJson(QJsonDocument::Compact);
    if (file.write(json) != json.size() || !file.commit()) return false;
    m_generation = nextGeneration;
    return true;
}

bool HnswEmbeddingIndex::loadFromDisk(QString* errorMessage) {
    QWriteLocker locker(&m_lock);
    resetLocked();
    if (!m_provider || m_provider->dimension() <= 0) return false;
    QFile file(metaFilePath());
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QJsonObject meta = QJsonDocument::fromJson(file.readAll()).object();
    if (meta.value(QStringLiteral("model")).toString() != m_provider->modelName() ||
        meta.value(QStringLiteral("m")).toInt() != m_params.m ||
        meta.value(QStringLiteral("efConstruction")).toInt() != m_params.efConstruction ||
        meta.value(QStringLiteral("dimension")).toInt() != m_provider->dimension() ||
        meta.value(QStringLiteral("distance")).toString() != QStringLiteral("cosine") ||
        meta.value(QStringLiteral("contentHashRule")).toString() != QStringLiteral("sha1-utf8")) return false;
    QFile binary(indexFilePath());
    if (!binary.open(QIODevice::ReadOnly)) return false;
    QCryptographicHash digest(QCryptographicHash::Sha256);
    if (!digest.addData(&binary) || QString::fromLatin1(digest.result().toHex()) !=
        meta.value(QStringLiteral("binarySha256")).toString()) return false;
    m_dimension = meta.value(QStringLiteral("dimension")).toInt();
    if (!ensureIndexAllocated(m_dimension)) return false;
    try { m_index->loadIndex(indexFilePath().toStdString(), m_space.get()); } catch (...) {
        m_index.reset();
        if (errorMessage) *errorMessage = QStringLiteral("hnsw load failed");
        return false;
    }
    m_index->setEf(m_params.efSearch);
    m_generation = meta.value(QStringLiteral("generation")).toInteger();
    m_tombstones = static_cast<int>(m_index->getDeletedCount());
    if (!loadLabelMapsFromDatabase() ||
        m_activeLabelByMemory.size() != meta.value(QStringLiteral("activeCount")).toInt(-1) ||
        m_activeLabelByMemory.size() != static_cast<int>(m_index->getCurrentElementCount()) - m_tombstones) {
        m_index.reset();
        m_activeLabelByMemory.clear();
        m_memoryByLabel.clear();
        m_hashByMemory.clear();
        return false;
    }
    try {
        for (auto it = m_memoryByLabel.cbegin(); it != m_memoryByLabel.cend(); ++it)
            m_index->getDataByLabel<float>(static_cast<hnswlib::labeltype>(it.key()));
    } catch (...) {
        m_index.reset();
        m_activeLabelByMemory.clear();
        m_memoryByLabel.clear();
        m_hashByMemory.clear();
        return false;
    }
    return true;
}

bool HnswEmbeddingIndex::rebuildFromRepository(QString* errorMessage) {
    QWriteLocker locker(&m_lock);
    resetLocked();
    if (errorMessage) errorMessage->clear();
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    bool inSavepoint = false;
    const QString savepoint = QStringLiteral("hnsw_rebuild_") + QUuid::createUuid().toString(QUuid::Id128);
    const auto fail = [&](const QString& message) {
        if (inSavepoint) {
            QSqlQuery rollback(db);
            rollback.exec(QStringLiteral("ROLLBACK TO SAVEPOINT %1").arg(savepoint));
            rollback.exec(QStringLiteral("RELEASE SAVEPOINT %1").arg(savepoint));
        }
        resetLocked();
        if (errorMessage) *errorMessage = message;
        return false;
    };
    if (!db.isOpen() || !m_provider || m_provider->dimension() <= 0)
        return fail(QStringLiteral("rebuild database or provider unavailable"));
    QSqlQuery transaction(db);
    if (!transaction.exec(QStringLiteral("SAVEPOINT %1").arg(savepoint)))
        return fail(QStringLiteral("cannot start rebuild savepoint"));
    inSavepoint = true;
    try {
        if (!ensureIndexAllocated(m_provider->dimension()))
            return fail(QStringLiteral("invalid index configuration"));
        // Retain numeric assignments but reactivate only rows actually rebuilt.
        QSqlQuery deactivate(db);
        deactivate.prepare(QStringLiteral("UPDATE memory_hnsw_labels SET status='Deleted' WHERE model=:model"));
        deactivate.bindValue(QStringLiteral(":model"), m_provider->modelName());
        if (!deactivate.exec()) return fail(QStringLiteral("cannot synchronize index labels"));
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "SELECT e.memory_id,e.dimension,e.vector_blob,e.content_hash "
            "FROM memory_embeddings e JOIN memory_items i ON i.id=e.memory_id "
            "WHERE e.model=:model AND i.status='active' AND i.privacy_level IN ('public','personal') "
            "AND i.partition!='hippocampus' AND i.type NOT IN ('working','short_term','task_shadow') "
            "AND (i.expires_at IS NULL OR i.expires_at='' OR julianday(i.expires_at)>julianday('now'))"));
        q.bindValue(QStringLiteral(":model"), m_provider->modelName());
        if (!q.exec()) return fail(QStringLiteral("cannot read authoritative vectors"));
        while (q.next()) {
            const int dimension = q.value(1).toInt();
            const QByteArray blob = q.value(2).toByteArray();
            if (dimension != m_dimension || blob.size() != qint64(dimension) * sizeof(float)) continue;
            QVector<float> vector(dimension);
            std::memcpy(vector.data(), blob.constData(), blob.size());
            if (!normalize(vector)) continue;
            if (!insertVectorLocked(q.value(0).toString(), vector, q.value(3).toString()))
                return fail(QStringLiteral("cannot insert rebuilt vector"));
        }
        if (q.lastError().isValid()) return fail(QStringLiteral("authoritative vector read failed"));
        q.finish();
        if (!saveToDiskLocked(errorMessage)) return fail(QStringLiteral("cannot persist rebuilt index"));
        if (!transaction.exec(QStringLiteral("RELEASE SAVEPOINT %1").arg(savepoint)))
            return fail(QStringLiteral("cannot commit rebuilt labels"));
        inSavepoint = false;
        return true;
    } catch (const std::exception&) {
        return fail(QStringLiteral("HNSW rebuild failed"));
    }
}

bool HnswEmbeddingIndex::isReady() const { QReadLocker l(&m_lock); return m_index != nullptr; }
int HnswEmbeddingIndex::activeCount() const { QReadLocker l(&m_lock); return m_activeLabelByMemory.size(); }
int HnswEmbeddingIndex::tombstoneCount() const { QReadLocker l(&m_lock); return m_tombstones; }
double HnswEmbeddingIndex::tombstoneRatio() const { QReadLocker l(&m_lock); const int total = m_activeLabelByMemory.size() + m_tombstones; return total ? static_cast<double>(m_tombstones) / total : 0.0; }
bool HnswEmbeddingIndex::needsCompaction() const { return tombstoneRatio() >= m_params.tombstoneRebuildRatio; }
