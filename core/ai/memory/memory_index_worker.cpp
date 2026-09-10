#include "memory_index_worker.h"
#include "hnsw_embedding_index.h"

#include <QDateTime>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QVariant>
#include <cmath>

namespace {
QString utcNow() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODate); }
}

MemoryIndexWorker::MemoryIndexWorker(HnswEmbeddingIndex& index) : m_index(index) {}

bool MemoryIndexWorker::ensureReady() {
    if (!m_index.provider() || m_index.provider()->dimension() <= 0) return false;
    return m_index.isReady() || m_index.loadFromDisk() || m_index.rebuildFromRepository();
}

int MemoryIndexWorker::processPending(int limit) {
    if (limit <= 0) return 0;
    QSqlDatabase db = QSqlDatabase::database(m_index.connectionName(), false);
    if (!db.isOpen()) return 0;
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT id FROM memory_index_jobs WHERE status IN ('Pending','Processing') "
                            "ORDER BY created_at LIMIT :limit"));
    q.bindValue(QStringLiteral(":limit"), limit);
    if (!q.exec()) return 0;
    QStringList ids;
    while (q.next()) ids.append(q.value(0).toString());
    q.finish();
    if (ids.isEmpty()) {
        ensureReady();
        return 0;
    }
    int completed = 0;
    for (const QString& id : ids) {
        if (!processOne(id)) break;
        ++completed;
    }
    return completed;
}

bool MemoryIndexWorker::processOne(const QString& jobId) {
    QSqlDatabase db = QSqlDatabase::database(m_index.connectionName(), false);
    if (!db.isOpen()) return false;
    if (!m_index.provider() || m_index.provider()->dimension() <= 0) return false;
    QSqlQuery claim(db);
    claim.prepare(QStringLiteral("UPDATE memory_index_jobs SET status='Processing', attempt_count=attempt_count+1, updated_at=:ts "
                                "WHERE id=:id AND status IN ('Pending','Processing')"));
    claim.bindValue(QStringLiteral(":ts"), utcNow()); claim.bindValue(QStringLiteral(":id"), jobId);
    if (!claim.exec() || claim.numRowsAffected() != 1) return false;

    QSqlQuery read(db);
    read.prepare(QStringLiteral("SELECT memory_id,operation FROM memory_index_jobs WHERE id=:id"));
    read.bindValue(QStringLiteral(":id"), jobId);
    if (!read.exec() || !read.next()) return false;
    const QString memoryId = read.value(0).toString();
    const QString operation = read.value(1).toString().toLower();

    const auto remove = [&]() {
        if (!m_index.removeVector(memoryId)) return false;
        QSqlQuery erase(db);
        erase.prepare(QStringLiteral("DELETE FROM memory_embeddings WHERE memory_id=:id"));
        erase.bindValue(QStringLiteral(":id"), memoryId);
        return erase.exec();
    };
    bool ok = ensureReady();
    if (ok && operation == QStringLiteral("delete")) {
        ok = remove();
    } else if (ok && operation == QStringLiteral("rebuild")) {
        ok = m_index.rebuildFromRepository();
    } else if (ok && operation == QStringLiteral("upsert")) {
        ok = false;
        QSqlQuery memory(db);
        memory.prepare(QStringLiteral(
            "SELECT summary,content,privacy_level,status,partition,type,"
            "(expires_at IS NULL OR expires_at='' OR julianday(expires_at)>julianday('now')) "
            "FROM memory_items WHERE id=:id"));
        memory.bindValue(QStringLiteral(":id"), memoryId);
        if (!memory.exec()) return false;
        if (memory.next()) {
            const QString privacy = memory.value(2).toString();
            const QString status = memory.value(3).toString();
            const QString partition = memory.value(4).toString();
            const QString type = memory.value(5).toString();
            if ((privacy == QStringLiteral("public") || privacy == QStringLiteral("personal")) &&
                status == QStringLiteral("active") && !partition.isEmpty() && partition != QStringLiteral("hippocampus") &&
                type != QStringLiteral("working") && type != QStringLiteral("short_term") &&
                type != QStringLiteral("task_shadow") && memory.value(6).toBool()) {
                const QString text = memory.value(0).toString() + QStringLiteral("\n") + memory.value(1).toString();
                const auto vector = m_index.provider()->embed(text);
                double norm = 0.0;
                for (float value : vector) norm += static_cast<double>(value) * value;
                if (vector.size() == m_index.provider()->dimension() && std::isfinite(norm) && norm > 1e-12) {
                    QSqlQuery persist(db);
                    persist.prepare(QStringLiteral(
                        "INSERT OR REPLACE INTO memory_embeddings(memory_id,model,dimension,vector_blob,content_hash,updated_at) "
                        "VALUES(:id,:model,:dim,:blob,:hash,:ts)"));
                    persist.bindValue(QStringLiteral(":id"), memoryId);
                    persist.bindValue(QStringLiteral(":model"), m_index.provider()->modelName());
                    persist.bindValue(QStringLiteral(":dim"), vector.size());
                    persist.bindValue(QStringLiteral(":blob"), QByteArray(reinterpret_cast<const char*>(vector.constData()), vector.size() * sizeof(float)));
                    persist.bindValue(QStringLiteral(":hash"), HnswEmbeddingIndex::contentHash(text));
                    persist.bindValue(QStringLiteral(":ts"), utcNow());
                    ok = persist.exec() && m_index.upsertVector(memoryId, vector, HnswEmbeddingIndex::contentHash(text));
                }
            } else {
                ok = remove();
            }
        } else {
            ok = remove();
        }
    } else {
        ok = false;
    }

    if (ok) ok = m_index.saveToDisk();

    QSqlQuery finish(db);
    finish.prepare(QStringLiteral("UPDATE memory_index_jobs SET status=:status,updated_at=:ts WHERE id=:id"));
    finish.bindValue(QStringLiteral(":status"), ok ? QStringLiteral("Completed") : QStringLiteral("Pending"));
    finish.bindValue(QStringLiteral(":ts"), utcNow()); finish.bindValue(QStringLiteral(":id"), jobId);
    return finish.exec() && ok;
}
