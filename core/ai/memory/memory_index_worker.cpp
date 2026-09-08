#include "memory_index_worker.h"
#include "hnsw_embedding_index.h"

#include <QDateTime>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QVariant>

namespace {
QString utcNow() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODate); }
}

MemoryIndexWorker::MemoryIndexWorker(HnswEmbeddingIndex& index) : m_index(index) {}

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
    int completed = 0;
    for (const QString& id : ids) completed += processOne(id) ? 1 : 0;
    return completed;
}

bool MemoryIndexWorker::processOne(const QString& jobId) {
    QSqlDatabase db = QSqlDatabase::database(m_index.connectionName(), false);
    if (!db.isOpen()) return false;
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

    bool ok = false;
    if (operation == QStringLiteral("delete")) {
        ok = m_index.removeVector(memoryId);
    } else if (operation == QStringLiteral("rebuild")) {
        ok = m_index.rebuildFromRepository();
    } else if (operation == QStringLiteral("upsert")) {
        QSqlQuery memory(db);
        memory.prepare(QStringLiteral("SELECT summary,content,privacy_level,status,partition FROM memory_items WHERE id=:id"));
        memory.bindValue(QStringLiteral(":id"), memoryId);
        if (memory.exec() && memory.next()) {
            const QString privacy = memory.value(2).toString();
            const QString status = memory.value(3).toString();
            const QString partition = memory.value(4).toString();
            if (privacy != QStringLiteral("sensitive") && status == QStringLiteral("active") && partition != QStringLiteral("hippocampus")) {
                const QString text = memory.value(0).toString() + QStringLiteral("\n") + memory.value(1).toString();
                ok = m_index.upsert(memoryId, text);
            } else {
                ok = m_index.removeVector(memoryId);
            }
        } else {
            ok = m_index.removeVector(memoryId);
        }
    }

    QSqlQuery finish(db);
    finish.prepare(QStringLiteral("UPDATE memory_index_jobs SET status=:status,updated_at=:ts WHERE id=:id"));
    finish.bindValue(QStringLiteral(":status"), ok ? QStringLiteral("Completed") : QStringLiteral("Pending"));
    finish.bindValue(QStringLiteral(":ts"), utcNow()); finish.bindValue(QStringLiteral(":id"), jobId);
    return finish.exec() && ok;
}
