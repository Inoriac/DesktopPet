#include "profile_data_migrator.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QMap>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUrl>
#include <QUuid>

#include "ai/memory/memory_store.h"

namespace {

QString newConnectionName() {
    return QStringLiteral("profile_migration_%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

bool isCanonicalProfileId(const QString& profileId) {
    const QString candidate = profileId.trimmed();
    const QUuid uuid = QUuid::fromString(candidate);
    return !uuid.isNull()
        && uuid.toString(QUuid::WithoutBraces).toLower() == candidate;
}

QString quotedIdentifier(QString identifier) {
    identifier.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QStringLiteral("\"") + identifier + QStringLiteral("\"");
}

void removeTemporaryDatabase(const QString& path) {
    QFile::remove(path);
    QFile::remove(path + QStringLiteral("-wal"));
    QFile::remove(path + QStringLiteral("-shm"));
}

bool integrityCheck(const QString& path, QString* diagnostic) {
    const QString connectionName = newConnectionName();
    bool valid = false;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        database.setDatabaseName(path);
        if (!database.open()) {
            if (diagnostic) *diagnostic = database.lastError().text();
        } else {
            QSqlQuery query(database);
            if (!query.exec(QStringLiteral("PRAGMA integrity_check"))
                || !query.next()
                || query.value(0).toString() != QStringLiteral("ok")
                || query.next()) {
                if (diagnostic) {
                    *diagnostic = query.lastError().isValid()
                        ? query.lastError().text()
                        : QStringLiteral("SQLite integrity_check did not return exactly one ok row");
                }
            } else {
                valid = true;
            }
            database.close();
        }
    }
    QSqlDatabase::removeDatabase(connectionName);
    return valid;
}

bool tableCounts(const QString& path, QMap<QString, qint64>* counts, QString* diagnostic) {
    const QString connectionName = newConnectionName();
    bool success = false;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        database.setDatabaseName(path);
        if (!database.open()) {
            if (diagnostic) *diagnostic = database.lastError().text();
        } else {
            QSqlQuery tables(database);
            if (!tables.exec(QStringLiteral(
                    "SELECT name FROM sqlite_master "
                    "WHERE type = 'table' AND name NOT LIKE 'sqlite_%' ORDER BY name"))) {
                if (diagnostic) *diagnostic = tables.lastError().text();
            } else {
                success = true;
                while (tables.next()) {
                    const QString name = tables.value(0).toString();
                    QSqlQuery countQuery(database);
                    if (!countQuery.exec(QStringLiteral("SELECT COUNT(*) FROM %1")
                                             .arg(quotedIdentifier(name)))
                        || !countQuery.next()) {
                        if (diagnostic) *diagnostic = countQuery.lastError().text();
                        success = false;
                        break;
                    }
                    counts->insert(name, countQuery.value(0).toLongLong());
                }
            }
            database.close();
        }
    }
    QSqlDatabase::removeDatabase(connectionName);
    return success;
}

bool completeProfileDatabase(const QString& path, QString* diagnostic) {
    QMap<QString, qint64> counts;
    if (!integrityCheck(path, diagnostic) || !tableCounts(path, &counts, diagnostic)) {
        return false;
    }
    if (!counts.contains(QStringLiteral("memory_items"))) {
        if (diagnostic) *diagnostic = QStringLiteral("profile database has no memory_items table");
        return false;
    }
    return true;
}

bool checkpointLegacyDatabase(const QString& path, QString* diagnostic) {
    const QString connectionName = newConnectionName();
    bool success = false;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_URI"));
        QString databaseUri = QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath())
                                  .toString(QUrl::FullyEncoded);
        // SQLite URI mode=rw opens read-write but fails instead of creating a missing source DB.
        databaseUri.append(QStringLiteral("?mode=rw"));
        database.setDatabaseName(databaseUri);
        if (!database.open()) {
            if (diagnostic) *diagnostic = database.lastError().text();
        } else {
            QSqlQuery query(database);
            if (query.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"))
                && query.next()
                && query.value(0).toInt() == 0) {
                success = true;
            } else if (diagnostic) {
                *diagnostic = query.lastError().isValid()
                    ? query.lastError().text()
                    : QStringLiteral("legacy SQLite WAL checkpoint is busy");
            }
            database.close();
        }
    }
    QSqlDatabase::removeDatabase(connectionName);
    return success;
}

std::optional<qint64> uniqueLegacyJsonEntryCount(
    const QString& path, QString* diagnostic) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (diagnostic) *diagnostic = file.errorString();
        return std::nullopt;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        if (diagnostic) *diagnostic = QStringLiteral("legacy memory JSON is not a valid array");
        return std::nullopt;
    }
    QSet<QString> ids;
    for (const QJsonValue& value : document.array()) {
        if (!value.isObject()) continue;
        const QString id = value.toObject().value(QStringLiteral("id")).toString();
        if (id.isEmpty()) continue;
        if (ids.contains(id)) {
            if (diagnostic) *diagnostic = QStringLiteral("legacy memory JSON contains duplicate ids");
            return std::nullopt;
        }
        ids.insert(id);
    }
    return ids.size();
}

ProfileMigrationResult readyResult(ProfileMigrationStatus status,
                                   const QString& databasePath,
                                   const QString& jsonPath) {
    return {status, databasePath, jsonPath, QString()};
}

ProfileMigrationResult legacyResult(ProfileMigrationStatus status,
                                    const ProfileMigrationRequest& request,
                                    const QString& diagnostic) {
    return {status, request.legacyDatabasePath, request.legacyJsonPath, diagnostic};
}

} // namespace

bool ProfileMigrationResult::profileStoreReady() const {
    return status == ProfileMigrationStatus::NoLegacyData
        || status == ProfileMigrationStatus::AlreadyMigrated
        || status == ProfileMigrationStatus::Migrated;
}

ProfileMigrationResult ProfileDataMigrator::migrateLegacyMemory(
    const ProfileMigrationRequest& request) const {
    if (!isCanonicalProfileId(request.profileId) || request.appDataRoot.trimmed().isEmpty()) {
        return legacyResult(ProfileMigrationStatus::Failed, request,
                            QStringLiteral("profile migration request is invalid"));
    }

    QSet<QString> registeredIds;
    for (const QString& registeredProfileId : request.registeredProfileIds) {
        if (!isCanonicalProfileId(registeredProfileId)
            || registeredIds.contains(registeredProfileId)) {
            return legacyResult(ProfileMigrationStatus::Failed, request,
                                QStringLiteral("registered profile ids are invalid or duplicated"));
        }
        registeredIds.insert(registeredProfileId);
    }
    if (!registeredIds.contains(request.profileId)) {
        return legacyResult(ProfileMigrationStatus::Failed, request,
                            QStringLiteral("migration profile is not present in the registry"));
    }
    if (request.confirmedLegacyOwnerProfileId.has_value()
        && (!isCanonicalProfileId(*request.confirmedLegacyOwnerProfileId)
            || !registeredIds.contains(*request.confirmedLegacyOwnerProfileId))) {
        return legacyResult(ProfileMigrationStatus::Failed, request,
                            QStringLiteral("confirmed legacy owner is not a registered profile"));
    }

    const QDir appData(request.appDataRoot);
    const QString profilesRoot = appData.filePath(QStringLiteral("profiles"));
    const QString profileRoot = QDir(profilesRoot).filePath(request.profileId);
    const QString targetDatabase = QDir(profileRoot).filePath(QStringLiteral("memory.db"));
    const QString targetJson = QDir(profileRoot).filePath(QStringLiteral("memory.json"));

    if (QFile::exists(targetDatabase)) {
        QString diagnostic;
        if (completeProfileDatabase(targetDatabase, &diagnostic)) {
            return readyResult(ProfileMigrationStatus::AlreadyMigrated,
                               targetDatabase, targetJson);
        }
        return legacyResult(ProfileMigrationStatus::Failed, request,
                            QStringLiteral("existing profile database is invalid: %1")
                                .arg(diagnostic));
    }

    const bool uniqueProfile = registeredIds.size() == 1 && registeredIds.contains(request.profileId);
    const bool confirmedOwner = request.confirmedLegacyOwnerProfileId.has_value()
        && *request.confirmedLegacyOwnerProfileId == request.profileId;

    if (!QDir().mkpath(profilesRoot) || !QDir().mkpath(profileRoot)) {
        return legacyResult(ProfileMigrationStatus::Failed, request,
                            QStringLiteral("failed to create profile data directory"));
    }

    QLockFile migrationLock(QDir(profilesRoot).filePath(
        QStringLiteral(".legacy-memory-migration.lock")));
    migrationLock.setStaleLockTime(30000);
    if (!migrationLock.tryLock(5000)) {
        return legacyResult(ProfileMigrationStatus::Failed, request,
                            QStringLiteral("timed out waiting for legacy memory migration lock"));
    }

    if (QFile::exists(targetDatabase)) {
        QString diagnostic;
        if (completeProfileDatabase(targetDatabase, &diagnostic)) {
            return readyResult(ProfileMigrationStatus::AlreadyMigrated,
                               targetDatabase, targetJson);
        }
        return legacyResult(ProfileMigrationStatus::Failed, request,
                            QStringLiteral("existing profile database is invalid: %1")
                                .arg(diagnostic));
    }

    const bool hasLegacyDatabase = QFile::exists(request.legacyDatabasePath);
    const bool hasLegacyJson = QFile::exists(request.legacyJsonPath);
    if (!hasLegacyDatabase && !hasLegacyJson) {
        return readyResult(ProfileMigrationStatus::NoLegacyData,
                           targetDatabase, targetJson);
    }
    if (!uniqueProfile && !confirmedOwner) {
        return legacyResult(ProfileMigrationStatus::Ambiguous, request,
                            QStringLiteral("legacy memory owner is ambiguous"));
    }

    const QString temporaryDatabase = targetDatabase + QStringLiteral(".tmp");
    removeTemporaryDatabase(temporaryDatabase);
    QString diagnostic;

    if (hasLegacyDatabase) {
        if (!checkpointLegacyDatabase(request.legacyDatabasePath, &diagnostic)
            || !QFile::copy(request.legacyDatabasePath, temporaryDatabase)) {
            removeTemporaryDatabase(temporaryDatabase);
            if (diagnostic.isEmpty()) diagnostic = QStringLiteral("failed to copy legacy database");
            return legacyResult(ProfileMigrationStatus::Failed, request, diagnostic);
        }

        QMap<QString, qint64> sourceCounts;
        QMap<QString, qint64> temporaryCounts;
        if (!integrityCheck(temporaryDatabase, &diagnostic)
            || !tableCounts(request.legacyDatabasePath, &sourceCounts, &diagnostic)
            || !tableCounts(temporaryDatabase, &temporaryCounts, &diagnostic)
            || !sourceCounts.contains(QStringLiteral("memory_items"))
            || sourceCounts != temporaryCounts) {
            removeTemporaryDatabase(temporaryDatabase);
            if (diagnostic.isEmpty()) diagnostic = QStringLiteral("legacy database row counts differ");
            return legacyResult(ProfileMigrationStatus::Failed, request, diagnostic);
        }
    } else {
        const std::optional<qint64> expectedCount =
            uniqueLegacyJsonEntryCount(request.legacyJsonPath, &diagnostic);
        if (!expectedCount) {
            return legacyResult(ProfileMigrationStatus::Failed, request, diagnostic);
        }
        bool imported = false;
        {
            MemoryStore store;
            store.setDatabasePath(temporaryDatabase);
            imported = store.importLegacyJson(request.legacyJsonPath, &diagnostic);
        }
        if (!imported) {
            removeTemporaryDatabase(temporaryDatabase);
            return legacyResult(ProfileMigrationStatus::Failed, request, diagnostic);
        }
        if (!checkpointLegacyDatabase(temporaryDatabase, &diagnostic)) {
            removeTemporaryDatabase(temporaryDatabase);
            return legacyResult(ProfileMigrationStatus::Failed, request, diagnostic);
        }
        QMap<QString, qint64> counts;
        if (!integrityCheck(temporaryDatabase, &diagnostic)
            || !tableCounts(temporaryDatabase, &counts, &diagnostic)
            || counts.value(QStringLiteral("memory_items"), -1) != *expectedCount) {
            removeTemporaryDatabase(temporaryDatabase);
            if (diagnostic.isEmpty()) diagnostic = QStringLiteral("legacy JSON row count differs");
            return legacyResult(ProfileMigrationStatus::Failed, request, diagnostic);
        }
    }

    if (!QFile::rename(temporaryDatabase, targetDatabase)) {
        removeTemporaryDatabase(temporaryDatabase);
        return legacyResult(ProfileMigrationStatus::Failed, request,
                            QStringLiteral("failed to atomically activate profile database"));
    }
    return readyResult(ProfileMigrationStatus::Migrated, targetDatabase, targetJson);
}
