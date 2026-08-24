#include "event_outbox.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

#include "event_schema_registry.h"
#include "sqlite_event_repository.h"

#include <utility>

namespace {

QString compactJson(const QJsonObject& object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

QString newConnectionName() {
    return QStringLiteral("event_outbox_%1").arg(
        QUuid::createUuid().toString(QUuid::WithoutBraces));
}

DomainError outboxError(const QString& message, const QSqlError& error = {}) {
    return domainError(QStringLiteral("EVENT_OUTBOX_UNAVAILABLE"),
                       error.isValid() ? message + QStringLiteral(": ") + error.text()
                                       : message);
}

} // namespace

SqliteEventOutbox::SqliteEventOutbox(QString databasePath,
                                     const EventSchemaRegistry* schemas,
                                     QString profileId)
    : m_databasePath(std::move(databasePath))
    , m_schemas(schemas)
    , m_profileId(std::move(profileId)) {}

Result<QString, DomainError> SqliteEventOutbox::enqueue(
    RuntimeUnitOfWork& unitOfWork, const EventDraft& draft) {
    if (!m_schemas || !unitOfWork.isActive()
        || !QSqlDatabase::contains(unitOfWork.connectionName())) {
        return Result<QString, DomainError>::failure(
            outboxError(QStringLiteral("runtime unit of work is unavailable")));
    }
    QSqlDatabase database = QSqlDatabase::database(unitOfWork.connectionName());
    if (QFileInfo(database.databaseName()).absoluteFilePath()
            != QFileInfo(m_databasePath).absoluteFilePath()) {
        return Result<QString, DomainError>::failure(
            outboxError(QStringLiteral("runtime unit of work belongs to another database")));
    }

    EventDraft finalDraft = draft;
    if (finalDraft.profileId != m_profileId) {
        return Result<QString, DomainError>::failure(
            outboxError(QStringLiteral("outbox event belongs to another profile")));
    }
    if (finalDraft.eventId.isEmpty()) {
        finalDraft.eventId = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
    }
    if (!finalDraft.occurredAt.isValid()) {
        finalDraft.occurredAt = QDateTime::currentDateTimeUtc();
    } else {
        finalDraft.occurredAt = finalDraft.occurredAt.toUTC();
    }
    const Result<void, DomainError> validation = m_schemas->validate(finalDraft);
    if (!validation.isOk()) {
        return Result<QString, DomainError>::failure(validation.error());
    }

    const QString outboxId = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "INSERT INTO event_outbox(outbox_id,event_id,payload_json,status,created_at) "
        "VALUES(?,?,?,'Pending',?)"));
    query.addBindValue(outboxId);
    query.addBindValue(finalDraft.eventId);
    query.addBindValue(compactJson(eventDraftToJson(finalDraft)));
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    if (!query.exec()) {
        return Result<QString, DomainError>::failure(
            outboxError(QStringLiteral("failed to enqueue event"), query.lastError()));
    }
    return Result<QString, DomainError>::success(outboxId);
}

Result<int, DomainError> SqliteEventOutbox::dispatchPending(int limit) {
    if (!m_schemas || limit <= 0) {
        return Result<int, DomainError>::failure(
            outboxError(QStringLiteral("outbox dispatch request is invalid")));
    }
    const QString name = newConnectionName();
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
    database.setDatabaseName(m_databasePath);
    if (!database.open()) {
        const DomainError error = outboxError(
            QStringLiteral("failed to open outbox database"), database.lastError());
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(name);
        return Result<int, DomainError>::failure(error);
    }
    QSqlQuery pragma(database);
    pragma.exec(QStringLiteral("PRAGMA foreign_keys=ON"));
    pragma.exec(QStringLiteral("PRAGMA busy_timeout=5000"));

    QSqlQuery pending(database);
    pending.prepare(QStringLiteral(
        "SELECT outbox_id,payload_json,attempt_count FROM event_outbox "
        "WHERE status='Pending' AND (next_attempt_at IS NULL OR next_attempt_at<=?) "
        "ORDER BY created_at,outbox_id LIMIT ?"));
    pending.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    pending.addBindValue(limit);
    if (!pending.exec()) {
        const DomainError error = outboxError(
            QStringLiteral("failed to scan event outbox"), pending.lastError());
        pending = QSqlQuery();
        pragma = QSqlQuery();
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(name);
        return Result<int, DomainError>::failure(error);
    }

    struct PendingRow { QString id; QByteArray payload; int attempts = 0; };
    QList<PendingRow> rows;
    while (pending.next()) {
        rows.append({pending.value(0).toString(), pending.value(1).toByteArray(),
                     pending.value(2).toInt()});
    }
    pending = QSqlQuery();

    int delivered = 0;
    DomainError failure;
    bool failed = false;
    for (const PendingRow& row : rows) {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(row.payload, &parseError);
        Result<EventDraft, DomainError> parsed = document.isObject()
            ? eventDraftFromJson(document.object())
            : Result<EventDraft, DomainError>::failure(
                outboxError(QStringLiteral("outbox payload is invalid JSON")));
        if (!parsed.isOk()) {
            failure = parsed.error();
            failed = true;
        } else {
            EventDraft draft = parsed.takeValue();
            if (!isCanonicalEventUuid(draft.eventId)
                || !draft.occurredAt.isValid()
                || draft.occurredAt.timeSpec() != Qt::UTC) {
                failure = domainError(
                    QStringLiteral("EVT_SCHEMA_INVALID"),
                    QStringLiteral("outbox event envelope is invalid"));
                failed = true;
            } else {
                const Result<void, DomainError> validation = m_schemas->validate(draft);
                if (!validation.isOk()) {
                    failure = validation.error();
                    failed = true;
                } else if (!database.transaction()) {
                    failure = outboxError(QStringLiteral("failed to begin outbox delivery"),
                                          database.lastError());
                    failed = true;
                } else {
                    auto inserted = SqliteEventRepository::insertEvent(database, draft, true);
                    QSqlQuery mark(database);
                    mark.prepare(QStringLiteral(
                        "UPDATE event_outbox SET status='Delivered',delivered_at=? WHERE outbox_id=?"));
                    mark.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
                    mark.addBindValue(row.id);
                    if (!inserted.isOk() || !mark.exec() || mark.numRowsAffected() != 1
                        || !database.commit()) {
                        failure = inserted.isOk()
                            ? outboxError(QStringLiteral("failed to mark outbox delivered"),
                                          mark.lastError().isValid() ? mark.lastError()
                                                                     : database.lastError())
                            : inserted.error();
                        database.rollback();
                        failed = true;
                    } else {
                        ++delivered;
                    }
                }
            }
        }
        if (failed) {
            const int nextAttempt = row.attempts + 1;
            QSqlQuery retry(database);
            retry.prepare(QStringLiteral(
                "UPDATE event_outbox SET attempt_count=?,next_attempt_at=? "
                "WHERE outbox_id=? AND status='Pending'"));
            retry.addBindValue(nextAttempt);
            retry.addBindValue(QDateTime::currentDateTimeUtc()
                .addSecs(qMin(3600, 1 << qMin(nextAttempt, 10)))
                .toString(Qt::ISODateWithMs));
            retry.addBindValue(row.id);
            const bool retryUpdated = retry.exec() && retry.numRowsAffected() == 1;
            if (!retryUpdated) {
                const QString retryError = retry.lastError().isValid()
                    ? retry.lastError().text()
                    : QStringLiteral("retry metadata update affected no pending row");
                failure.details.insert(QStringLiteral("retryMetadataAdvanced"), false);
                failure.details.insert(QStringLiteral("retryMetadataError"), retryError);
            }
            break;
        }
    }

    pending = QSqlQuery();
    pragma = QSqlQuery();
    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(name);
    if (failed) return Result<int, DomainError>::failure(failure);
    return Result<int, DomainError>::success(delivered);
}
