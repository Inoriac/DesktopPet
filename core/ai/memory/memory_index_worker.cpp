#include "memory_index_worker.h"
#include "hnsw_embedding_index.h"

#include <QDateTime>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSet>
#include <QUuid>
#include <QVariant>
#include <cmath>
#include <cstring>

namespace {
QString utcNow() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODate); }
qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }
qint64 retryDelayMs(int attempt) {
    const int exponent = qBound(3, attempt + 2, 16);
    return qMin<qint64>(600000, qint64(1) << exponent) * 1000;
}

QString eligibleMemorySql(const QString& selectClause,
                          const QString& suffix = QString()) {
    return selectClause + QStringLiteral(
        " FROM memory_items m "
        "WHERE (m.privacy_level='public' OR m.privacy_level='personal') "
        "AND m.status='active' "
        "AND m.partition IS NOT NULL AND m.partition<>'' "
        "AND m.partition NOT IN ('hippocampus','working','short_term') "
        "AND m.type NOT IN ('working','short_term','task_shadow') "
        "AND (m.expires_at IS NULL OR m.expires_at='' OR julianday(m.expires_at)>julianday('now')) ")
        + suffix;
}
}

MemoryIndexWorker::MemoryIndexWorker(HnswEmbeddingIndex& index) : m_index(index) {}

bool MemoryIndexWorker::ensureReady() {
    if (!m_index.provider() || m_index.provider()->dimension() <= 0) return false;
    return m_index.isReady() || m_index.loadFromDisk() || m_index.rebuildFromRepository();
}

int MemoryIndexWorker::enqueueBackfillJobs(int limit) {
    if (limit <= 0 || !m_index.provider() || m_index.provider()->dimension() <= 0) return 0;
    QSqlDatabase db = QSqlDatabase::database(m_index.connectionName(), false);
    if (!db.isOpen()) return 0;

    QSqlQuery candidates(db);
    candidates.prepare(eligibleMemorySql(QStringLiteral(
        "SELECT m.id,m.summary,m.content"), QStringLiteral(
        "AND NOT EXISTS ("
        "  SELECT 1 FROM memory_embeddings e WHERE e.memory_id=m.id AND e.model=:model"
        ") "
        "ORDER BY COALESCE(m.updated_at,m.created_at) DESC, m.id DESC LIMIT :limit")));
    candidates.bindValue(QStringLiteral(":model"), m_index.provider()->modelName());
    candidates.bindValue(QStringLiteral(":limit"), limit);
    if (!candidates.exec()) return 0;

    int queued = 0;
    while (candidates.next()) {
        const QString memoryId = candidates.value(0).toString();
        const QString text = candidates.value(1).toString() + QStringLiteral("\n") + candidates.value(2).toString();
        const QString hash = HnswEmbeddingIndex::contentHash(text);

        QSqlQuery existing(db);
        existing.prepare(QStringLiteral(
            "SELECT 1 FROM memory_index_jobs "
            "WHERE memory_id=:id AND status IN ('Pending','Processing') "
            "AND (model='' OR model=:model) LIMIT 1"));
        existing.bindValue(QStringLiteral(":id"), memoryId);
        existing.bindValue(QStringLiteral(":model"), m_index.provider()->modelName());
        if (existing.exec() && existing.next()) continue;

        QSqlQuery insert(db);
        insert.prepare(QStringLiteral(
            "INSERT INTO memory_index_jobs(id,memory_id,operation,model,content_hash,status,created_at,updated_at,next_attempt_at,last_error) "
            "VALUES(:job,:memory,'upsert',:model,:hash,'Pending',:created,:updated,0,'')"));
        const QString now = utcNow();
        insert.bindValue(QStringLiteral(":job"), QUuid::createUuid().toString(QUuid::WithoutBraces));
        insert.bindValue(QStringLiteral(":memory"), memoryId);
        insert.bindValue(QStringLiteral(":model"), m_index.provider()->modelName());
        insert.bindValue(QStringLiteral(":hash"), hash);
        insert.bindValue(QStringLiteral(":created"), now);
        insert.bindValue(QStringLiteral(":updated"), now);
        if (insert.exec()) ++queued;
    }
    return queued;
}

IndexCoverageStats MemoryIndexWorker::coverageStats(int scanLimit) const {
    IndexCoverageStats stats;
    if (!m_index.provider() || m_index.provider()->dimension() <= 0 || scanLimit <= 0) return stats;
    QSqlDatabase db = QSqlDatabase::database(m_index.connectionName(), false);
    if (!db.isOpen()) return stats;

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT m.id,m.summary,m.content,e.content_hash,"
        "EXISTS(SELECT 1 FROM memory_index_jobs j WHERE j.memory_id=m.id AND j.status IN ('Pending','Processing') "
        "AND (j.model='' OR j.model=:model2)) "
        "FROM memory_items m "
        "LEFT JOIN memory_embeddings e ON e.memory_id=m.id AND e.model=:model "
        "WHERE (m.privacy_level='public' OR m.privacy_level='personal') "
        "AND m.status='active' "
        "AND m.partition IS NOT NULL AND m.partition<>'' "
        "AND m.partition NOT IN ('hippocampus','working','short_term') "
        "AND m.type NOT IN ('working','short_term','task_shadow') "
        "AND (m.expires_at IS NULL OR m.expires_at='' OR julianday(m.expires_at)>julianday('now')) "
        "ORDER BY COALESCE(m.updated_at,m.created_at) DESC, m.id DESC LIMIT :limit"));
    query.bindValue(QStringLiteral(":model"), m_index.provider()->modelName());
    query.bindValue(QStringLiteral(":model2"), m_index.provider()->modelName());
    query.bindValue(QStringLiteral(":limit"), scanLimit);
    if (!query.exec()) return stats;
    while (query.next()) {
        ++stats.eligible;
        const QString text = query.value(1).toString() + QStringLiteral("\n") + query.value(2).toString();
        if (query.value(3).toString() == HnswEmbeddingIndex::contentHash(text)) ++stats.indexed;
        if (query.value(4).toBool()) ++stats.pending;
    }
    return stats;
}

bool MemoryIndexWorker::recordHealthSample(bool healthy,
                                           const QString& error,
                                           const QDate& day) {
    if (!m_index.provider() || !day.isValid()) return false;
    QSqlDatabase db = QSqlDatabase::database(m_index.connectionName(), false);
    if (!db.isOpen()) return false;
    const IndexCoverageStats stats = coverageStats();
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "INSERT INTO memory_index_health_daily(model,day,eligible_count,indexed_count,pending_count,healthy,failure_count,last_error,updated_at) "
        "VALUES(:model,:day,:eligible,:indexed,:pending,:healthy,:failure,:error,:updated) "
        "ON CONFLICT(model,day) DO UPDATE SET "
        "eligible_count=excluded.eligible_count,indexed_count=excluded.indexed_count,"
        "pending_count=excluded.pending_count,"
        "healthy=CASE WHEN memory_index_health_daily.healthy=1 AND excluded.healthy=1 THEN 1 ELSE 0 END,"
        "failure_count=memory_index_health_daily.failure_count+excluded.failure_count,"
        "last_error=excluded.last_error,updated_at=excluded.updated_at"));
    query.bindValue(QStringLiteral(":model"), m_index.provider()->modelName());
    query.bindValue(QStringLiteral(":day"), day.toString(Qt::ISODate));
    query.bindValue(QStringLiteral(":eligible"), stats.eligible);
    query.bindValue(QStringLiteral(":indexed"), stats.indexed);
    query.bindValue(QStringLiteral(":pending"), stats.pending);
    query.bindValue(QStringLiteral(":healthy"), healthy ? 1 : 0);
    query.bindValue(QStringLiteral(":failure"), healthy ? 0 : 1);
    query.bindValue(QStringLiteral(":error"), error.left(512));
    query.bindValue(QStringLiteral(":updated"), utcNow());
    return query.exec();
}

IndexRetirementGateStatus MemoryIndexWorker::retirementGateStatus(int requiredDays,
                                                                  double minimumCoverage,
                                                                  const QDate& today) const {
    IndexRetirementGateStatus status;
    status.requiredDays = std::max(1, requiredDays);
    status.minimumCoverage = minimumCoverage;
    status.worstCoverage = 1.0;
    if (!m_index.provider() || !today.isValid()) return status;
    QSqlDatabase db = QSqlDatabase::database(m_index.connectionName(), false);
    if (!db.isOpen()) return status;

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT day,eligible_count,indexed_count,healthy FROM memory_index_health_daily "
        "WHERE model=:model AND day>=:start AND day<=:end ORDER BY day DESC"));
    query.bindValue(QStringLiteral(":model"), m_index.provider()->modelName());
    query.bindValue(QStringLiteral(":start"), today.addDays(1 - status.requiredDays).toString(Qt::ISODate));
    query.bindValue(QStringLiteral(":end"), today.toString(Qt::ISODate));
    if (!query.exec()) return status;

    QSet<QString> days;
    while (query.next()) {
        const QString dayKey = query.value(0).toString();
        if (days.contains(dayKey)) continue;
        days.insert(dayKey);
        const int eligible = query.value(1).toInt();
        const int indexed = query.value(2).toInt();
        const double coverage = eligible > 0 ? static_cast<double>(indexed) / eligible : 1.0;
        status.worstCoverage = std::min(status.worstCoverage, coverage);
        if (query.value(3).toInt() == 1 && coverage >= minimumCoverage) ++status.healthyDays;
    }
    if (days.size() < status.requiredDays) status.worstCoverage = 0.0;
    status.canRetireLegacyScan = days.size() >= status.requiredDays
        && status.healthyDays >= status.requiredDays
        && status.worstCoverage >= minimumCoverage;
    return status;
}

int MemoryIndexWorker::processPending(int limit) {
    if (limit <= 0) return 0;
    QSqlDatabase db = QSqlDatabase::database(m_index.connectionName(), false);
    if (!db.isOpen()) return 0;
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT id FROM memory_index_jobs "
        "WHERE status IN ('Pending','Processing') AND next_attempt_at <= :now "
        "ORDER BY created_at LIMIT :limit"));
    q.bindValue(QStringLiteral(":now"), nowMs());
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
        if (processOne(id)) ++completed;
    }
    return completed;
}

bool MemoryIndexWorker::processOne(const QString& jobId) {
    QSqlDatabase db = QSqlDatabase::database(m_index.connectionName(), false);
    if (!db.isOpen() || !m_index.provider() || m_index.provider()->dimension() <= 0) return false;

    QSqlQuery read(db);
    read.prepare(QStringLiteral(
        "SELECT memory_id,operation,model,content_hash,attempt_count,next_attempt_at "
        "FROM memory_index_jobs WHERE id=:id AND status IN ('Pending','Processing')"));
    read.bindValue(QStringLiteral(":id"), jobId);
    if (!read.exec() || !read.next()) return false;
    const QString memoryId = read.value(0).toString();
    const QString jobModel = read.value(2).toString();
    const QString jobHash = read.value(3).toString();
    const int previousAttempts = read.value(4).toInt();
    if (read.value(5).toLongLong() > nowMs()) return false;
    if (!jobModel.isEmpty() && jobModel != m_index.provider()->modelName()) return false;

    QSqlQuery claim(db);
    claim.prepare(QStringLiteral(
        "UPDATE memory_index_jobs SET status='Processing',attempt_count=attempt_count+1,updated_at=:ts "
        "WHERE id=:id AND status IN ('Pending','Processing') AND next_attempt_at<=:now"));
    claim.bindValue(QStringLiteral(":ts"), utcNow());
    claim.bindValue(QStringLiteral(":now"), nowMs());
    claim.bindValue(QStringLiteral(":id"), jobId);
    if (!claim.exec() || claim.numRowsAffected() != 1) return false;

    auto finish = [&](bool ok, const QString& error, const QString& hash) {
        QSqlQuery done(db);
        done.prepare(QStringLiteral(
            "UPDATE memory_index_jobs SET status=:status,model=:model,content_hash=:hash,"
            "next_attempt_at=:next,last_error=:error,updated_at=:ts WHERE id=:id"));
        const int attempt = previousAttempts + 1;
        const bool success = ok;
        done.bindValue(QStringLiteral(":status"), success ? QStringLiteral("Completed") : QStringLiteral("Pending"));
        done.bindValue(QStringLiteral(":model"), m_index.provider()->modelName());
        done.bindValue(QStringLiteral(":hash"), hash);
        done.bindValue(QStringLiteral(":next"), success ? 0 : nowMs() + retryDelayMs(attempt));
        done.bindValue(QStringLiteral(":error"), error.left(512));
        done.bindValue(QStringLiteral(":ts"), utcNow());
        done.bindValue(QStringLiteral(":id"), jobId);
        return done.exec() && success;
    };

    if (!ensureReady()) return finish(false, QStringLiteral("index unavailable"), jobHash);

    QSqlQuery memory(db);
    memory.prepare(QStringLiteral(
        "SELECT summary,content,privacy_level,status,partition,type,"
        "(expires_at IS NULL OR expires_at='' OR julianday(expires_at)>julianday('now')) "
        "FROM memory_items WHERE id=:id"));
    memory.bindValue(QStringLiteral(":id"), memoryId);
    if (!memory.exec()) return finish(false, memory.lastError().text(), jobHash);

    bool eligible = false;
    QString text;
    QString currentHash;
    if (memory.next()) {
        const QString privacy = memory.value(2).toString();
        const QString status = memory.value(3).toString();
        const QString partition = memory.value(4).toString();
        const QString type = memory.value(5).toString();
        eligible = (privacy == QStringLiteral("public") || privacy == QStringLiteral("personal")) &&
            status == QStringLiteral("active") && !partition.isEmpty() && partition != QStringLiteral("hippocampus") &&
            type != QStringLiteral("working") && type != QStringLiteral("short_term") &&
            type != QStringLiteral("task_shadow") && memory.value(6).toBool();
        text = memory.value(0).toString() + QStringLiteral("\n") + memory.value(1).toString();
        currentHash = HnswEmbeddingIndex::contentHash(text);
    }

    if (!eligible) {
        if (!m_index.removeVector(memoryId)) return finish(false, QStringLiteral("cannot remove ineligible memory"), currentHash);
        QSqlQuery erase(db);
        erase.prepare(QStringLiteral("DELETE FROM memory_embeddings WHERE memory_id=:id AND model=:model"));
        erase.bindValue(QStringLiteral(":id"), memoryId);
        erase.bindValue(QStringLiteral(":model"), m_index.provider()->modelName());
        if (!erase.exec()) return finish(false, erase.lastError().text(), currentHash);
        if (!m_index.saveToDisk()) return finish(false, QStringLiteral("cannot save delete"), currentHash);
        return finish(true, {}, currentHash);
    }

    if (!jobHash.isEmpty() && jobHash == currentHash) {
        QSqlQuery cached(db);
        cached.prepare(QStringLiteral("SELECT dimension,vector_blob FROM memory_embeddings WHERE memory_id=:id AND model=:model AND content_hash=:hash"));
        cached.bindValue(QStringLiteral(":id"), memoryId);
        cached.bindValue(QStringLiteral(":model"), m_index.provider()->modelName());
        cached.bindValue(QStringLiteral(":hash"), currentHash);
        if (cached.exec() && cached.next()) {
            const int dimension = cached.value(0).toInt();
            const QByteArray blob = cached.value(1).toByteArray();
            if (dimension == m_index.provider()->dimension() && blob.size() == dimension * static_cast<int>(sizeof(float))) {
                QVector<float> cachedVector(dimension);
                std::memcpy(cachedVector.data(), blob.constData(), blob.size());
                if (m_index.upsertVector(memoryId, cachedVector, currentHash) && m_index.saveToDisk())
                    return finish(true, {}, currentHash);
            }
        }
    }

    const QVector<float> vector = m_index.provider()->embed(text);
    double norm = 0.0;
    for (float value : vector) norm += static_cast<double>(value) * value;
    if (vector.size() != m_index.provider()->dimension() || !std::isfinite(norm) || norm <= 1e-12)
        return finish(false, QStringLiteral("invalid embedding"), currentHash);

    QSqlQuery current(db);
    current.prepare(QStringLiteral("SELECT summary,content,privacy_level,status,updated_at FROM memory_items WHERE id=:id"));
    current.bindValue(QStringLiteral(":id"), memoryId);
    if (!current.exec() || !current.next()) return finish(false, QStringLiteral("memory changed during embedding"), currentHash);
    const QString verifyText = current.value(0).toString() + QStringLiteral("\n") + current.value(1).toString();
    if (HnswEmbeddingIndex::contentHash(verifyText) != currentHash ||
        current.value(2).toString() == QStringLiteral("sensitive") ||
        current.value(3).toString() != QStringLiteral("active"))
        return finish(false, QStringLiteral("memory changed during embedding"), currentHash);

    QSqlQuery persist(db);
    persist.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO memory_embeddings(memory_id,model,dimension,vector_blob,content_hash,updated_at) "
        "VALUES(:id,:model,:dim,:blob,:hash,:ts)"));
    persist.bindValue(QStringLiteral(":id"), memoryId);
    persist.bindValue(QStringLiteral(":model"), m_index.provider()->modelName());
    persist.bindValue(QStringLiteral(":dim"), vector.size());
    persist.bindValue(QStringLiteral(":blob"), QByteArray(reinterpret_cast<const char*>(vector.constData()), vector.size() * sizeof(float)));
    persist.bindValue(QStringLiteral(":hash"), currentHash);
    persist.bindValue(QStringLiteral(":ts"), utcNow());
    if (!persist.exec() || !m_index.upsertVector(memoryId, vector, currentHash) || !m_index.saveToDisk())
        return finish(false, QStringLiteral("cannot persist embedding/index"), currentHash);
    return finish(true, {}, currentHash);
}
