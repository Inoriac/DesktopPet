#include "sqlite_identity_repository.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

namespace {

DomainError identityStoreError(const QString& message,
                               const QSqlError& error = {}) {
    return domainError(
        QStringLiteral("IDENTITY_STORE_UNAVAILABLE"),
        error.isValid() ? message + QStringLiteral(": ") + error.text() : message);
}

DomainError versionConflict(const QString& message) {
    return domainError(QStringLiteral("STATE_VERSION_CONFLICT"), message);
}

QString compactJson(const QJsonObject& object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

QString compactJson(const QJsonArray& array) {
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

QDateTime utcOrNow(const QDateTime& value) {
    return value.isValid() ? value.toUTC() : QDateTime::currentDateTimeUtc();
}

QString dateToStorage(const QDateTime& value) {
    return utcOrNow(value).toString(Qt::ISODateWithMs);
}

QDateTime dateFromStorage(const QString& value) {
    return QDateTime::fromString(value, Qt::ISODateWithMs).toUTC();
}

Result<QJsonObject, DomainError> parseObject(const QString& value,
                                             const QString& field) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(value.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return Result<QJsonObject, DomainError>::failure(
            identityStoreError(field + QStringLiteral(" contains invalid JSON")));
    }
    return Result<QJsonObject, DomainError>::success(document.object());
}

Result<QJsonArray, DomainError> parseArray(const QString& value,
                                           const QString& field) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(value.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        return Result<QJsonArray, DomainError>::failure(
            identityStoreError(field + QStringLiteral(" contains invalid JSON")));
    }
    return Result<QJsonArray, DomainError>::success(document.array());
}

Result<TraitEvidence, DomainError> evidenceFromQuery(const QSqlQuery& query) {
    const std::optional<TraitEvidenceStatus> status =
        traitEvidenceStatusFromString(query.value(QStringLiteral("status")).toString());
    if (!status.has_value()) {
        return Result<TraitEvidence, DomainError>::failure(
            identityStoreError(QStringLiteral("trait evidence status is invalid")));
    }
    TraitEvidence evidence;
    evidence.evidenceId = query.value(QStringLiteral("evidence_id")).toString();
    evidence.profileId = query.value(QStringLiteral("profile_id")).toString();
    evidence.trait = query.value(QStringLiteral("trait")).toString();
    evidence.sourceEventId = query.value(QStringLiteral("source_event_id")).toString();
    evidence.direction = query.value(QStringLiteral("direction")).toDouble();
    evidence.weight = query.value(QStringLiteral("weight")).toDouble();
    evidence.confidence = query.value(QStringLiteral("confidence")).toDouble();
    evidence.contextKey = query.value(QStringLiteral("context_key")).toString();
    evidence.status = *status;
    evidence.createdAt = dateFromStorage(
        query.value(QStringLiteral("created_at")).toString());
    return Result<TraitEvidence, DomainError>::success(std::move(evidence));
}

Result<PersonalitySnapshot, DomainError> personalityFromQuery(
    const QSqlQuery& query) {
    auto baselineJson = parseObject(
        query.value(QStringLiteral("baseline_json")).toString(),
        QStringLiteral("personality baseline"));
    if (!baselineJson.isOk()) {
        return Result<PersonalitySnapshot, DomainError>::failure(baselineJson.error());
    }
    auto tendencyJson = parseObject(
        query.value(QStringLiteral("tendency_json")).toString(),
        QStringLiteral("personality tendency"));
    if (!tendencyJson.isOk()) {
        return Result<PersonalitySnapshot, DomainError>::failure(tendencyJson.error());
    }

    PersonalitySnapshot snapshot;
    snapshot.stateId = query.value(QStringLiteral("state_id")).toString();
    snapshot.profileId = query.value(QStringLiteral("profile_id")).toString();
    snapshot.version = query.value(QStringLiteral("version")).toLongLong();
    snapshot.baseline = IdentityBaseline::fromJson(baselineJson.value());
    const QJsonObject state = tendencyJson.value();
    snapshot.tendencies = numericMapFromJson(
        state.value(QStringLiteral("tendencies")).isObject()
            ? state.value(QStringLiteral("tendencies")).toObject()
            : state);
    const QJsonArray sources = state.value(
        QStringLiteral("sourceEvidenceIds")).toArray();
    for (const QJsonValue& source : sources) {
        const QString id = source.toString().trimmed();
        if (!id.isEmpty() && !snapshot.sourceEvidenceIds.contains(id)) {
            snapshot.sourceEvidenceIds.append(id);
        }
    }
    snapshot.evidenceCutoffSequence = query.value(
        QStringLiteral("evidence_cutoff_sequence")).toLongLong();
    snapshot.effectiveAt = dateFromStorage(
        query.value(QStringLiteral("effective_at")).toString());
    snapshot.createdAt = dateFromStorage(
        query.value(QStringLiteral("created_at")).toString());
    return Result<PersonalitySnapshot, DomainError>::success(std::move(snapshot));
}

Result<RelationshipSnapshot, DomainError> relationshipFromQuery(
    const QSqlQuery& query) {
    auto stateJson = parseObject(
        query.value(QStringLiteral("state_json")).toString(),
        QStringLiteral("relationship state"));
    if (!stateJson.isOk()) {
        return Result<RelationshipSnapshot, DomainError>::failure(stateJson.error());
    }
    RelationshipSnapshot snapshot = relationshipSnapshotStateFromJson(
        stateJson.value());
    snapshot.stateId = query.value(QStringLiteral("state_id")).toString();
    snapshot.profileId = query.value(QStringLiteral("profile_id")).toString();
    snapshot.subjectId = query.value(QStringLiteral("subject_id")).toString();
    snapshot.version = query.value(QStringLiteral("version")).toLongLong();
    snapshot.evidenceCutoffSequence = query.value(
        QStringLiteral("evidence_cutoff_sequence")).toLongLong();
    snapshot.effectiveAt = dateFromStorage(
        query.value(QStringLiteral("effective_at")).toString());
    snapshot.createdAt = dateFromStorage(
        query.value(QStringLiteral("created_at")).toString());
    return Result<RelationshipSnapshot, DomainError>::success(std::move(snapshot));
}

Result<SelfModelSnapshot, DomainError> selfModelFromQuery(const QSqlQuery& query) {
    auto narrativeJson = parseObject(
        query.value(QStringLiteral("narrative_json")).toString(),
        QStringLiteral("self model narrative"));
    if (!narrativeJson.isOk()) {
        return Result<SelfModelSnapshot, DomainError>::failure(narrativeJson.error());
    }
    auto evidenceJson = parseArray(
        query.value(QStringLiteral("evidence_json")).toString(),
        QStringLiteral("self model evidence"));
    if (!evidenceJson.isOk()) {
        return Result<SelfModelSnapshot, DomainError>::failure(evidenceJson.error());
    }
    SelfModelSnapshot snapshot;
    snapshot.versionId = query.value(QStringLiteral("version_id")).toString();
    snapshot.profileId = query.value(QStringLiteral("profile_id")).toString();
    const QString parent = query.value(QStringLiteral("parent_version_id")).toString();
    if (!parent.isEmpty()) snapshot.parentVersionId = parent;
    snapshot.narrative = narrativeJson.value().value(
        QStringLiteral("narrative")).toString();
    snapshot.evidence = evidenceSetFromJson(evidenceJson.value());
    snapshot.effectiveAt = dateFromStorage(
        query.value(QStringLiteral("effective_at")).toString());
    snapshot.createdAt = dateFromStorage(
        query.value(QStringLiteral("created_at")).toString());
    return Result<SelfModelSnapshot, DomainError>::success(std::move(snapshot));
}

} // namespace

SqliteIdentityRepository::~SqliteIdentityRepository() {
    close();
}

Result<void, DomainError> SqliteIdentityRepository::open(
    const QString& databasePath) {
    close();
    if (databasePath.trimmed().isEmpty()) {
        return Result<void, DomainError>::failure(
            identityStoreError(QStringLiteral("identity database path is empty")));
    }

    m_connectionName = QStringLiteral("identity_repository_%1").arg(
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase database = QSqlDatabase::addDatabase(
        QStringLiteral("QSQLITE"), m_connectionName);
    database.setDatabaseName(databasePath);
    if (!database.open()) {
        const DomainError error = identityStoreError(
            QStringLiteral("failed to open identity database"), database.lastError());
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connectionName);
        m_connectionName.clear();
        return Result<void, DomainError>::failure(error);
    }
    QSqlQuery pragma(database);
    if (!pragma.exec(QStringLiteral("PRAGMA foreign_keys=ON"))
        || !pragma.exec(QStringLiteral("PRAGMA busy_timeout=5000"))) {
        const DomainError error = identityStoreError(
            QStringLiteral("failed to configure identity database"), pragma.lastError());
        pragma = QSqlQuery();
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connectionName);
        m_connectionName.clear();
        return Result<void, DomainError>::failure(error);
    }
    for (const QString& table : {
             QStringLiteral("personality_state"),
             QStringLiteral("relationship_state"),
             QStringLiteral("self_model_version"),
             QStringLiteral("trait_evidence")}) {
        QSqlQuery schema(database);
        if (!schema.exec(QStringLiteral("SELECT 1 FROM %1 LIMIT 1").arg(table))) {
            const DomainError error = identityStoreError(
                QStringLiteral("identity schema is incomplete"), schema.lastError());
            schema = QSqlQuery();
            pragma = QSqlQuery();
            database.close();
            database = QSqlDatabase();
            QSqlDatabase::removeDatabase(m_connectionName);
            m_connectionName.clear();
            return Result<void, DomainError>::failure(error);
        }
    }
    m_databasePath = QFileInfo(databasePath).absoluteFilePath();
    return Result<void, DomainError>::success();
}

void SqliteIdentityRepository::close() {
    if (m_connectionName.isEmpty()) return;
    if (QSqlDatabase::contains(m_connectionName)) {
        {
            QSqlDatabase database = QSqlDatabase::database(m_connectionName, false);
            if (database.isValid()) database.close();
        }
        QSqlDatabase::removeDatabase(m_connectionName);
    }
    m_databasePath.clear();
    m_connectionName.clear();
}

bool SqliteIdentityRepository::isOpen() const {
    return !m_connectionName.isEmpty()
        && QSqlDatabase::contains(m_connectionName)
        && QSqlDatabase::database(m_connectionName, false).isOpen();
}

Result<void, DomainError> SqliteIdentityRepository::insertEvidence(
    const TraitEvidence& evidence) {
    if (!isOpen()) {
        return Result<void, DomainError>::failure(
            identityStoreError(QStringLiteral("identity repository is closed")));
    }
    QSqlDatabase database = QSqlDatabase::database(m_connectionName);
    if (!evidence.sourceEventId.isEmpty()) {
        QSqlQuery existing(database);
        existing.prepare(QStringLiteral(
            "SELECT evidence_id FROM trait_evidence "
            "WHERE profile_id=? AND trait=? AND source_event_id=? LIMIT 1"));
        existing.addBindValue(evidence.profileId);
        existing.addBindValue(evidence.trait);
        existing.addBindValue(evidence.sourceEventId);
        if (!existing.exec()) {
            return Result<void, DomainError>::failure(identityStoreError(
                QStringLiteral("failed to check duplicate trait evidence"),
                existing.lastError()));
        }
        if (existing.next()) return Result<void, DomainError>::success();
    }

    QSqlQuery insert(database);
    insert.prepare(QStringLiteral(
        "INSERT INTO trait_evidence(evidence_id,profile_id,trait,source_event_id,"
        "direction,weight,confidence,context_key,status,created_at) "
        "VALUES(?,?,?,?,?,?,?,?,?,?)"));
    insert.addBindValue(evidence.evidenceId);
    insert.addBindValue(evidence.profileId);
    insert.addBindValue(evidence.trait);
    insert.addBindValue(evidence.sourceEventId);
    insert.addBindValue(evidence.direction);
    insert.addBindValue(evidence.weight);
    insert.addBindValue(evidence.confidence);
    insert.addBindValue(evidence.contextKey);
    insert.addBindValue(traitEvidenceStatusToString(evidence.status));
    insert.addBindValue(dateToStorage(evidence.createdAt));
    if (!insert.exec()) {
        return Result<void, DomainError>::failure(identityStoreError(
            QStringLiteral("failed to insert trait evidence"), insert.lastError()));
    }
    return Result<void, DomainError>::success();
}

Result<QList<TraitEvidence>, DomainError>
SqliteIdentityRepository::evidenceBySource(
    const QString& profileId, const QString& sourceEventId) const {
    if (!isOpen()) {
        return Result<QList<TraitEvidence>, DomainError>::failure(
            identityStoreError(QStringLiteral("identity repository is closed")));
    }
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral(
        "SELECT * FROM trait_evidence WHERE profile_id=? AND source_event_id=? "
        "ORDER BY created_at,evidence_id"));
    query.addBindValue(profileId);
    query.addBindValue(sourceEventId);
    if (!query.exec()) {
        return Result<QList<TraitEvidence>, DomainError>::failure(identityStoreError(
            QStringLiteral("failed to read trait evidence"), query.lastError()));
    }
    QList<TraitEvidence> evidence;
    while (query.next()) {
        auto parsed = evidenceFromQuery(query);
        if (!parsed.isOk()) {
            return Result<QList<TraitEvidence>, DomainError>::failure(parsed.error());
        }
        evidence.append(parsed.takeValue());
    }
    return Result<QList<TraitEvidence>, DomainError>::success(std::move(evidence));
}

Result<QList<TraitEvidence>, DomainError>
SqliteIdentityRepository::pendingEvidence(
    const QString& profileId,
    const QDateTime& from,
    const QDateTime& to) const {
    if (!isOpen() || !from.isValid() || !to.isValid() || from > to) {
        return Result<QList<TraitEvidence>, DomainError>::failure(
            identityStoreError(QStringLiteral("pending evidence request is invalid")));
    }
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral(
        "SELECT * FROM trait_evidence WHERE profile_id=? AND status='Pending' "
        "AND created_at>=? AND created_at<=? ORDER BY created_at,evidence_id"));
    query.addBindValue(profileId);
    query.addBindValue(dateToStorage(from));
    query.addBindValue(dateToStorage(to));
    if (!query.exec()) {
        return Result<QList<TraitEvidence>, DomainError>::failure(identityStoreError(
            QStringLiteral("failed to read pending trait evidence"), query.lastError()));
    }
    QList<TraitEvidence> evidence;
    while (query.next()) {
        auto parsed = evidenceFromQuery(query);
        if (!parsed.isOk()) {
            return Result<QList<TraitEvidence>, DomainError>::failure(parsed.error());
        }
        evidence.append(parsed.takeValue());
    }
    return Result<QList<TraitEvidence>, DomainError>::success(std::move(evidence));
}

Result<std::optional<PersonalitySnapshot>, DomainError>
SqliteIdentityRepository::currentPersonality(const QString& profileId) const {
    if (!isOpen()) {
        return Result<std::optional<PersonalitySnapshot>, DomainError>::failure(
            identityStoreError(QStringLiteral("identity repository is closed")));
    }
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral(
        "SELECT * FROM personality_state WHERE profile_id=? "
        "ORDER BY version DESC LIMIT 1"));
    query.addBindValue(profileId);
    if (!query.exec()) {
        return Result<std::optional<PersonalitySnapshot>, DomainError>::failure(
            identityStoreError(QStringLiteral("failed to read current personality"),
                               query.lastError()));
    }
    if (!query.next()) {
        return Result<std::optional<PersonalitySnapshot>, DomainError>::success(std::nullopt);
    }
    auto parsed = personalityFromQuery(query);
    if (!parsed.isOk()) {
        return Result<std::optional<PersonalitySnapshot>, DomainError>::failure(parsed.error());
    }
    return Result<std::optional<PersonalitySnapshot>, DomainError>::success(
        parsed.takeValue());
}

Result<std::optional<PersonalitySnapshot>, DomainError>
SqliteIdentityRepository::personalityAt(
    const QString& profileId, qint64 version) const {
    if (!isOpen()) {
        return Result<std::optional<PersonalitySnapshot>, DomainError>::failure(
            identityStoreError(QStringLiteral("identity repository is closed")));
    }
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral(
        "SELECT * FROM personality_state WHERE profile_id=? AND version=? LIMIT 1"));
    query.addBindValue(profileId);
    query.addBindValue(version);
    if (!query.exec()) {
        return Result<std::optional<PersonalitySnapshot>, DomainError>::failure(
            identityStoreError(QStringLiteral("failed to read personality version"),
                               query.lastError()));
    }
    if (!query.next()) {
        return Result<std::optional<PersonalitySnapshot>, DomainError>::success(std::nullopt);
    }
    auto parsed = personalityFromQuery(query);
    if (!parsed.isOk()) {
        return Result<std::optional<PersonalitySnapshot>, DomainError>::failure(parsed.error());
    }
    return Result<std::optional<PersonalitySnapshot>, DomainError>::success(
        parsed.takeValue());
}

Result<std::optional<PersonalitySnapshot>, DomainError>
SqliteIdentityRepository::personalityByStateId(
    const QString& profileId, const QString& stateId) const {
    if (!isOpen()) {
        return Result<std::optional<PersonalitySnapshot>, DomainError>::failure(
            identityStoreError(QStringLiteral("identity repository is closed")));
    }
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral(
        "SELECT * FROM personality_state WHERE profile_id=? AND state_id=? LIMIT 1"));
    query.addBindValue(profileId);
    query.addBindValue(stateId);
    if (!query.exec()) {
        return Result<std::optional<PersonalitySnapshot>, DomainError>::failure(
            identityStoreError(QStringLiteral("failed to read personality state"),
                               query.lastError()));
    }
    if (!query.next()) {
        return Result<std::optional<PersonalitySnapshot>, DomainError>::success(std::nullopt);
    }
    auto parsed = personalityFromQuery(query);
    if (!parsed.isOk()) {
        return Result<std::optional<PersonalitySnapshot>, DomainError>::failure(parsed.error());
    }
    return Result<std::optional<PersonalitySnapshot>, DomainError>::success(
        parsed.takeValue());
}

Result<QList<PersonalitySnapshot>, DomainError>
SqliteIdentityRepository::personalityHistory(const QString& profileId) const {
    if (!isOpen()) {
        return Result<QList<PersonalitySnapshot>, DomainError>::failure(
            identityStoreError(QStringLiteral("identity repository is closed")));
    }
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral(
        "SELECT * FROM personality_state WHERE profile_id=? ORDER BY version"));
    query.addBindValue(profileId);
    if (!query.exec()) {
        return Result<QList<PersonalitySnapshot>, DomainError>::failure(
            identityStoreError(QStringLiteral("failed to read personality history"),
                               query.lastError()));
    }
    QList<PersonalitySnapshot> history;
    while (query.next()) {
        auto parsed = personalityFromQuery(query);
        if (!parsed.isOk()) {
            return Result<QList<PersonalitySnapshot>, DomainError>::failure(parsed.error());
        }
        history.append(parsed.takeValue());
    }
    return Result<QList<PersonalitySnapshot>, DomainError>::success(std::move(history));
}

Result<void, DomainError> SqliteIdentityRepository::appendPersonalityState(
    RuntimeUnitOfWork& unitOfWork,
    const PersonalitySnapshot& snapshot,
    const QStringList& consumedEvidenceIds) {
    if (!isOpen() || !unitOfWork.isActive()
        || !QSqlDatabase::contains(unitOfWork.connectionName())) {
        return Result<void, DomainError>::failure(
            identityStoreError(QStringLiteral("identity unit of work is unavailable")));
    }
    QSqlDatabase database = QSqlDatabase::database(unitOfWork.connectionName());
    if (QFileInfo(database.databaseName()).absoluteFilePath() != m_databasePath) {
        return Result<void, DomainError>::failure(
            identityStoreError(QStringLiteral("identity unit of work belongs to another database")));
    }

    QSqlQuery current(database);
    current.prepare(QStringLiteral(
        "SELECT COALESCE(MAX(version),0) FROM personality_state WHERE profile_id=?"));
    current.addBindValue(snapshot.profileId);
    if (!current.exec() || !current.next()) {
        return Result<void, DomainError>::failure(identityStoreError(
            QStringLiteral("failed to read personality CAS version"), current.lastError()));
    }
    if (current.value(0).toLongLong() != snapshot.version - 1) {
        return Result<void, DomainError>::failure(
            versionConflict(QStringLiteral("personality version changed")));
    }

    QJsonArray sources;
    for (const QString& source : snapshot.sourceEvidenceIds) sources.append(source);
    const QJsonObject tendencyState{
        {QStringLiteral("tendencies"), numericMapToJson(snapshot.tendencies)},
        {QStringLiteral("sourceEvidenceIds"), sources}
    };
    QSqlQuery insert(database);
    insert.prepare(QStringLiteral(
        "INSERT INTO personality_state(state_id,profile_id,version,baseline_json,"
        "tendency_json,evidence_cutoff_sequence,effective_at,created_at) "
        "VALUES(?,?,?,?,?,?,?,?)"));
    insert.addBindValue(snapshot.stateId);
    insert.addBindValue(snapshot.profileId);
    insert.addBindValue(snapshot.version);
    insert.addBindValue(compactJson(snapshot.baseline.toJson()));
    insert.addBindValue(compactJson(tendencyState));
    insert.addBindValue(snapshot.evidenceCutoffSequence);
    insert.addBindValue(dateToStorage(snapshot.effectiveAt));
    insert.addBindValue(dateToStorage(snapshot.createdAt));
    if (!insert.exec()) {
        return Result<void, DomainError>::failure(identityStoreError(
            QStringLiteral("failed to append personality state"), insert.lastError()));
    }

    for (const QString& evidenceId : consumedEvidenceIds) {
        QSqlQuery consume(database);
        consume.prepare(QStringLiteral(
            "UPDATE trait_evidence SET status='Consumed' "
            "WHERE evidence_id=? AND profile_id=? AND status='Pending'"));
        consume.addBindValue(evidenceId);
        consume.addBindValue(snapshot.profileId);
        if (!consume.exec() || consume.numRowsAffected() != 1) {
            return Result<void, DomainError>::failure(identityStoreError(
                QStringLiteral("failed to consume trait evidence"), consume.lastError()));
        }
    }
    return Result<void, DomainError>::success();
}

Result<std::optional<RelationshipSnapshot>, DomainError>
SqliteIdentityRepository::currentRelationship(
    const QString& profileId, const QString& subjectId) const {
    if (!isOpen()) {
        return Result<std::optional<RelationshipSnapshot>, DomainError>::failure(
            identityStoreError(QStringLiteral("identity repository is closed")));
    }
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral(
        "SELECT * FROM relationship_state WHERE profile_id=? AND subject_id=? "
        "ORDER BY version DESC LIMIT 1"));
    query.addBindValue(profileId);
    query.addBindValue(subjectId);
    if (!query.exec()) {
        return Result<std::optional<RelationshipSnapshot>, DomainError>::failure(
            identityStoreError(QStringLiteral("failed to read current relationship"),
                               query.lastError()));
    }
    if (!query.next()) {
        return Result<std::optional<RelationshipSnapshot>, DomainError>::success(std::nullopt);
    }
    auto parsed = relationshipFromQuery(query);
    if (!parsed.isOk()) {
        return Result<std::optional<RelationshipSnapshot>, DomainError>::failure(parsed.error());
    }
    return Result<std::optional<RelationshipSnapshot>, DomainError>::success(
        parsed.takeValue());
}

Result<std::optional<RelationshipSnapshot>, DomainError>
SqliteIdentityRepository::relationshipAt(
    const QString& profileId,
    const QString& subjectId,
    qint64 version) const {
    if (!isOpen()) {
        return Result<std::optional<RelationshipSnapshot>, DomainError>::failure(
            identityStoreError(QStringLiteral("identity repository is closed")));
    }
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral(
        "SELECT * FROM relationship_state WHERE profile_id=? AND subject_id=? "
        "AND version=? LIMIT 1"));
    query.addBindValue(profileId);
    query.addBindValue(subjectId);
    query.addBindValue(version);
    if (!query.exec()) {
        return Result<std::optional<RelationshipSnapshot>, DomainError>::failure(
            identityStoreError(QStringLiteral("failed to read relationship version"),
                               query.lastError()));
    }
    if (!query.next()) {
        return Result<std::optional<RelationshipSnapshot>, DomainError>::success(std::nullopt);
    }
    auto parsed = relationshipFromQuery(query);
    if (!parsed.isOk()) {
        return Result<std::optional<RelationshipSnapshot>, DomainError>::failure(parsed.error());
    }
    return Result<std::optional<RelationshipSnapshot>, DomainError>::success(
        parsed.takeValue());
}

Result<void, DomainError> SqliteIdentityRepository::appendRelationshipState(
    const RelationshipSnapshot& snapshot) {
    if (!isOpen()) {
        return Result<void, DomainError>::failure(
            identityStoreError(QStringLiteral("identity repository is closed")));
    }
    QSqlDatabase database = QSqlDatabase::database(m_connectionName);
    if (!database.transaction()) {
        return Result<void, DomainError>::failure(identityStoreError(
            QStringLiteral("failed to begin relationship transaction"),
            database.lastError()));
    }
    QSqlQuery current(database);
    current.prepare(QStringLiteral(
        "SELECT COALESCE(MAX(version),0) FROM relationship_state "
        "WHERE profile_id=? AND subject_id=?"));
    current.addBindValue(snapshot.profileId);
    current.addBindValue(snapshot.subjectId);
    if (!current.exec() || !current.next()) {
        database.rollback();
        return Result<void, DomainError>::failure(identityStoreError(
            QStringLiteral("failed to read relationship CAS version"),
            current.lastError()));
    }
    if (current.value(0).toLongLong() != snapshot.version - 1) {
        database.rollback();
        return Result<void, DomainError>::failure(
            versionConflict(QStringLiteral("relationship version changed")));
    }
    QSqlQuery insert(database);
    insert.prepare(QStringLiteral(
        "INSERT INTO relationship_state(state_id,profile_id,subject_id,version,"
        "state_json,evidence_cutoff_sequence,effective_at,created_at) "
        "VALUES(?,?,?,?,?,?,?,?)"));
    insert.addBindValue(snapshot.stateId);
    insert.addBindValue(snapshot.profileId);
    insert.addBindValue(snapshot.subjectId);
    insert.addBindValue(snapshot.version);
    insert.addBindValue(compactJson(relationshipSnapshotStateToJson(snapshot)));
    insert.addBindValue(snapshot.evidenceCutoffSequence);
    insert.addBindValue(dateToStorage(snapshot.effectiveAt));
    insert.addBindValue(dateToStorage(snapshot.createdAt));
    if (!insert.exec() || !database.commit()) {
        const DomainError error = identityStoreError(
            QStringLiteral("failed to append relationship state"),
            insert.lastError().isValid() ? insert.lastError() : database.lastError());
        database.rollback();
        return Result<void, DomainError>::failure(error);
    }
    return Result<void, DomainError>::success();
}

Result<std::optional<SelfModelSnapshot>, DomainError>
SqliteIdentityRepository::currentSelfModel(const QString& profileId) const {
    if (!isOpen()) {
        return Result<std::optional<SelfModelSnapshot>, DomainError>::failure(
            identityStoreError(QStringLiteral("identity repository is closed")));
    }
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral(
        "SELECT rowid,* FROM self_model_version WHERE profile_id=? "
        "ORDER BY created_at DESC,rowid DESC LIMIT 1"));
    query.addBindValue(profileId);
    if (!query.exec()) {
        return Result<std::optional<SelfModelSnapshot>, DomainError>::failure(
            identityStoreError(QStringLiteral("failed to read current self model"),
                               query.lastError()));
    }
    if (!query.next()) {
        return Result<std::optional<SelfModelSnapshot>, DomainError>::success(std::nullopt);
    }
    auto parsed = selfModelFromQuery(query);
    if (!parsed.isOk()) {
        return Result<std::optional<SelfModelSnapshot>, DomainError>::failure(parsed.error());
    }
    return Result<std::optional<SelfModelSnapshot>, DomainError>::success(
        parsed.takeValue());
}

Result<std::optional<SelfModelSnapshot>, DomainError>
SqliteIdentityRepository::selfModelAt(
    const QString& profileId, const QString& versionId) const {
    if (!isOpen()) {
        return Result<std::optional<SelfModelSnapshot>, DomainError>::failure(
            identityStoreError(QStringLiteral("identity repository is closed")));
    }
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral(
        "SELECT * FROM self_model_version WHERE profile_id=? AND version_id=? LIMIT 1"));
    query.addBindValue(profileId);
    query.addBindValue(versionId);
    if (!query.exec()) {
        return Result<std::optional<SelfModelSnapshot>, DomainError>::failure(
            identityStoreError(QStringLiteral("failed to read self model version"),
                               query.lastError()));
    }
    if (!query.next()) {
        return Result<std::optional<SelfModelSnapshot>, DomainError>::success(std::nullopt);
    }
    auto parsed = selfModelFromQuery(query);
    if (!parsed.isOk()) {
        return Result<std::optional<SelfModelSnapshot>, DomainError>::failure(parsed.error());
    }
    return Result<std::optional<SelfModelSnapshot>, DomainError>::success(
        parsed.takeValue());
}

Result<void, DomainError> SqliteIdentityRepository::appendSelfModelState(
    const SelfModelSnapshot& snapshot) {
    if (!isOpen()) {
        return Result<void, DomainError>::failure(
            identityStoreError(QStringLiteral("identity repository is closed")));
    }
    QSqlDatabase database = QSqlDatabase::database(m_connectionName);
    if (!database.transaction()) {
        return Result<void, DomainError>::failure(identityStoreError(
            QStringLiteral("failed to begin self model transaction"), database.lastError()));
    }
    QSqlQuery current(database);
    current.prepare(QStringLiteral(
        "SELECT version_id FROM self_model_version WHERE profile_id=? "
        "ORDER BY created_at DESC,rowid DESC LIMIT 1"));
    current.addBindValue(snapshot.profileId);
    if (!current.exec()) {
        database.rollback();
        return Result<void, DomainError>::failure(identityStoreError(
            QStringLiteral("failed to read self model parent"), current.lastError()));
    }
    const bool hasCurrent = current.next();
    const QString currentId = hasCurrent ? current.value(0).toString() : QString();
    if ((snapshot.parentVersionId.has_value() && currentId != *snapshot.parentVersionId)
        || (!snapshot.parentVersionId.has_value() && hasCurrent)) {
        database.rollback();
        return Result<void, DomainError>::failure(
            versionConflict(QStringLiteral("self model parent changed")));
    }

    QSqlQuery insert(database);
    insert.prepare(QStringLiteral(
        "INSERT INTO self_model_version(version_id,profile_id,parent_version_id,"
        "narrative_json,evidence_json,effective_at,created_at) VALUES(?,?,?,?,?,?,?)"));
    insert.addBindValue(snapshot.versionId);
    insert.addBindValue(snapshot.profileId);
    insert.addBindValue(snapshot.parentVersionId.has_value()
                            ? QVariant(*snapshot.parentVersionId) : QVariant());
    insert.addBindValue(compactJson(QJsonObject{
        {QStringLiteral("narrative"), snapshot.narrative}}));
    insert.addBindValue(compactJson(evidenceSetToJson(snapshot.evidence)));
    insert.addBindValue(dateToStorage(snapshot.effectiveAt));
    insert.addBindValue(dateToStorage(snapshot.createdAt));
    if (!insert.exec() || !database.commit()) {
        const DomainError error = identityStoreError(
            QStringLiteral("failed to append self model version"),
            insert.lastError().isValid() ? insert.lastError() : database.lastError());
        database.rollback();
        return Result<void, DomainError>::failure(error);
    }
    return Result<void, DomainError>::success();
}
