#include "memory_index_worker.h"
#include "hnsw_embedding_index.h"

#include <QDateTime>
#include <QFile>
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
    return qMin<qint64>(600000, (qint64(1) << exponent) * 1000);
}

QString vectorInspectionSql() {
    return QStringLiteral(
        "SELECT m.id,m.summary,m.content,e.content_hash,e.dimension,e.vector_blob,"
        "l.content_hash,l.status "
        "FROM memory_items m "
        "LEFT JOIN memory_embeddings e ON e.memory_id=m.id AND e.model=:model "
        "LEFT JOIN memory_hnsw_labels l ON l.memory_id=m.id AND l.model=:model "
        "WHERE (m.privacy_level='public' OR m.privacy_level='personal') "
        "AND m.status='active' AND m.partition IS NOT NULL AND m.partition<>'' "
        "AND m.partition NOT IN ('hippocampus','working','short_term') "
        "AND m.type NOT IN ('working','short_term','task_shadow') "
        "AND (m.expires_at IS NULL OR m.expires_at='' OR julianday(m.expires_at)>julianday('now')) ");
}

bool validIndexedRow(const QSqlQuery& row, int dimension) {
    const QString hash = HnswEmbeddingIndex::contentHash(
        row.value(1).toString() + QStringLiteral("\n") + row.value(2).toString());
    if (row.value(3).toString() != hash || row.value(4).toInt() != dimension ||
        row.value(6).toString() != hash || row.value(7).toString() != QStringLiteral("Active")) return false;
    const QByteArray blob = row.value(5).toByteArray();
    if (blob.size() != dimension * static_cast<qint64>(sizeof(float))) return false;
    double norm = 0.0;
    for (int i = 0; i < dimension; ++i) {
        float value;
        std::memcpy(&value, blob.constData() + i * sizeof(float), sizeof(float));
        norm += static_cast<double>(value) * value;
    }
    return std::isfinite(norm) && norm > 1e-12;
}
}

MemoryIndexWorker::MemoryIndexWorker(HnswEmbeddingIndex& index) : m_index(index) {}

bool MemoryIndexWorker::ensureReady() {
    if (!m_index.provider() || m_index.provider()->dimension() <= 0) return false;
    if (m_index.isReady() || m_index.loadFromDisk()) return true;
    QSqlQuery previous(QSqlDatabase::database(m_index.connectionName(), false));
    previous.prepare(QStringLiteral("SELECT 1 FROM memory_embeddings WHERE model=:model LIMIT 1"));
    previous.bindValue(QStringLiteral(":model"), m_index.provider()->modelName());
    const bool hadVectors = previous.exec() && previous.next();
    previous.finish();
    if (hadVectors || QFile::exists(m_index.indexFilePath()) || QFile::exists(m_index.metaFilePath()))
        recordHealthSample(false, QStringLiteral("index load failed; recovery required"));
    const bool recovered = m_index.rebuildFromRepository();
    if (!recovered) recordHealthSample(false, QStringLiteral("index recovery failed"));
    return recovered;
}

int MemoryIndexWorker::enqueueBackfillJobs(int limit) {
    if (limit <= 0 || !m_index.provider() || m_index.provider()->dimension() <= 0) return 0;
    QSqlDatabase db = QSqlDatabase::database(m_index.connectionName(), false);
    if (!db.isOpen()) return 0;

    QSqlQuery candidates(db);
    candidates.prepare(vectorInspectionSql() + QStringLiteral(
        "AND m.id>:cursor AND NOT EXISTS (SELECT 1 FROM memory_index_jobs j "
        "WHERE j.memory_id=m.id AND j.status IN ('Pending','Processing') "
        "AND (COALESCE(j.model,'')='' OR j.model=:model)) "
        "ORDER BY m.id LIMIT :limit"));
    candidates.bindValue(QStringLiteral(":cursor"), m_backfillCursor.isNull() ? QStringLiteral("") : m_backfillCursor);
    candidates.bindValue(QStringLiteral(":model"), m_index.provider()->modelName());
    candidates.bindValue(QStringLiteral(":limit"), limit);
    if (!candidates.exec()) return 0;

    struct Repair { QString id; QString hash; };
    QList<Repair> repairs;
    int scanned = 0;
    while (candidates.next()) {
        ++scanned;
        m_backfillCursor = candidates.value(0).toString();
        if (!validIndexedRow(candidates, m_index.provider()->dimension())) {
            repairs.append({m_backfillCursor, HnswEmbeddingIndex::contentHash(
                candidates.value(1).toString() + QStringLiteral("\n") + candidates.value(2).toString())});
        }
    }
    candidates.finish();
    if (scanned < limit) m_backfillCursor.clear();
    int queued = 0;
    for (const auto& repair : repairs) {
        const QString memoryId = repair.id;
        const QString hash = repair.hash;
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
    query.prepare(vectorInspectionSql() + QStringLiteral("ORDER BY m.id LIMIT :limit"));
    query.bindValue(QStringLiteral(":model"), m_index.provider()->modelName());
    query.bindValue(QStringLiteral(":limit"), static_cast<qint64>(scanLimit) + 1);
    if (!query.exec()) return stats;
    stats.valid = true;
    stats.complete = true;
    while (query.next()) {
        if (stats.eligible == scanLimit) { stats.complete = false; break; }
        ++stats.eligible;
        if (validIndexedRow(query, m_index.provider()->dimension())) ++stats.indexed;
    }
    if (query.lastError().isValid()) stats.valid = false;
    query.finish();
    QSqlQuery pending(db);
    pending.prepare(QStringLiteral("SELECT COUNT(*) FROM memory_index_jobs WHERE "
        "status IN ('Pending','Processing') AND (COALESCE(model,'')='' OR model=:model)"));
    pending.bindValue(QStringLiteral(":model"), m_index.provider()->modelName());
    if (pending.exec() && pending.next()) stats.pending = pending.value(0).toInt();
    else stats.valid = false;
    return stats;
}

bool MemoryIndexWorker::recordHealthSample(bool healthy,
                                           const QString& error,
                                           const QDate& day) {
    if (!m_index.provider() || !day.isValid()) return false;
    QSqlDatabase db = QSqlDatabase::database(m_index.connectionName(), false);
    if (!db.isOpen()) return false;
    const IndexCoverageStats stats = coverageStats();
    healthy = healthy && stats.valid && stats.complete && m_index.isReady();
    const QString sampleError = !error.isEmpty() ? error :
        (!healthy ? QStringLiteral("coverage incomplete, unreadable, or index unavailable") : QString());
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "INSERT INTO memory_index_health_daily(model,day,eligible_count,indexed_count,pending_count,healthy,failure_count,last_error,updated_at) "
        "VALUES(:model,:day,:eligible,:indexed,:pending,:healthy,:failure,:error,:updated) "
        "ON CONFLICT(model,day) DO UPDATE SET "
        "eligible_count=CASE WHEN COALESCE(1.0*excluded.indexed_count/NULLIF(excluded.eligible_count,0),1.0) "
        "<COALESCE(1.0*memory_index_health_daily.indexed_count/NULLIF(memory_index_health_daily.eligible_count,0),1.0) "
        "THEN excluded.eligible_count ELSE memory_index_health_daily.eligible_count END,"
        "indexed_count=CASE WHEN COALESCE(1.0*excluded.indexed_count/NULLIF(excluded.eligible_count,0),1.0) "
        "<COALESCE(1.0*memory_index_health_daily.indexed_count/NULLIF(memory_index_health_daily.eligible_count,0),1.0) "
        "THEN excluded.indexed_count ELSE memory_index_health_daily.indexed_count END,"
        "pending_count=excluded.pending_count,"
        "healthy=CASE WHEN memory_index_health_daily.healthy=1 AND excluded.healthy=1 THEN 1 ELSE 0 END,"
        "failure_count=memory_index_health_daily.failure_count+excluded.failure_count,"
        "last_error=CASE WHEN excluded.healthy=0 THEN excluded.last_error ELSE memory_index_health_daily.last_error END,"
        "updated_at=excluded.updated_at"));
    query.bindValue(QStringLiteral(":model"), m_index.provider()->modelName());
    query.bindValue(QStringLiteral(":day"), day.toString(Qt::ISODate));
    query.bindValue(QStringLiteral(":eligible"), stats.eligible);
    query.bindValue(QStringLiteral(":indexed"), stats.indexed);
    query.bindValue(QStringLiteral(":pending"), stats.pending);
    query.bindValue(QStringLiteral(":healthy"), healthy ? 1 : 0);
    query.bindValue(QStringLiteral(":failure"), healthy ? 0 : 1);
    query.bindValue(QStringLiteral(":error"), sampleError.left(512));
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
    if (!m_index.provider() || !today.isValid() || requiredDays <= 0 ||
        !std::isfinite(minimumCoverage) || minimumCoverage < 0.95 || minimumCoverage > 1.0) return status;
    QSqlDatabase db = QSqlDatabase::database(m_index.connectionName(), false);
    if (!db.isOpen()) return status;

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT day,eligible_count,indexed_count,healthy,failure_count FROM memory_index_health_daily "
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
        if (query.value(3).toInt() == 1 && query.value(4).toInt() == 0 && coverage >= minimumCoverage) ++status.healthyDays;
    }
    if (days.size() < status.requiredDays) status.worstCoverage = 0.0;
    const auto current = coverageStats();
    status.canRetireLegacyScan = m_index.isReady() && current.valid && current.complete
        && current.ratio() >= minimumCoverage && current.pending == 0
        && days.size() >= status.requiredDays
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
        "AND (COALESCE(model,'')='' OR model=:model) "
        "ORDER BY created_at,id LIMIT :limit"));
    q.bindValue(QStringLiteral(":model"), m_index.provider() ? m_index.provider()->modelName() : QString());
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

    // Repairs may have lost the label row while this process still holds the old
    // in-memory mapping. Rebuild mappings before consuming that repair.
    QSqlQuery label(db);
    label.prepare(QStringLiteral("SELECT EXISTS(SELECT 1 FROM memory_hnsw_labels WHERE memory_id=:id AND model=:model), "
        "EXISTS(SELECT 1 FROM memory_embeddings WHERE memory_id=:id AND model=:model)"));
    label.bindValue(QStringLiteral(":id"), memoryId);
    label.bindValue(QStringLiteral(":model"), m_index.provider()->modelName());
    if (!label.exec() || !label.next()) return finish(false, label.lastError().text(), currentHash);
    if (!label.value(0).toBool() && label.value(1).toBool() && !m_index.rebuildFromRepository())
        return finish(false, QStringLiteral("cannot restore label mapping"), currentHash);

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
