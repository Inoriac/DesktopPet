#include "diary_service.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QUuid>

#include <utility>

#include "ai/context/context_assembler.h"
#include "ai/event/event_ledger.h"
#include "ai/model/model_router.h"
#include "private_key_provider.h"
#include "private_psyche_crypto.h"
#include "sqlite_private_psyche_repository.h"

namespace {

QJsonObject diarySchema() {
    return {
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("required"), QJsonArray{
             QStringLiteral("body"), QStringLiteral("index")}},
        {QStringLiteral("properties"), QJsonObject{
             {QStringLiteral("body"), QJsonObject{
                  {QStringLiteral("type"), QStringLiteral("string")}}},
             {QStringLiteral("index"), QJsonObject{
                  {QStringLiteral("type"), QStringLiteral("object")}}}}}
    };
}

Result<QPair<QString, QJsonObject>, DomainError> parseDiary(
    const QString& content) {
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(content.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return Result<QPair<QString, QJsonObject>, DomainError>::failure(
            domainError(QStringLiteral("MODEL_OUTPUT_INVALID"),
                        QStringLiteral("diary output is not a JSON object")));
    }
    const QJsonObject object = document.object();
    const QString body = object.value(QStringLiteral("body"))
                             .toString().trimmed().left(12000);
    const QJsonObject index = object.value(QStringLiteral("index")).toObject();
    if (body.isEmpty()) {
        return Result<QPair<QString, QJsonObject>, DomainError>::failure(
            domainError(QStringLiteral("MODEL_OUTPUT_INVALID"),
                        QStringLiteral("diary body is empty")));
    }
    return Result<QPair<QString, QJsonObject>, DomainError>::success({body, index});
}

QJsonObject diaryEnvelope(const DiaryEntry& entry) {
    return {
        {QStringLiteral("body"), entry.body},
        {QStringLiteral("index"), entry.index},
        {QStringLiteral("localDate"), entry.localDate.toString(Qt::ISODate)},
        {QStringLiteral("sourceCutoffSequence"),
         static_cast<double>(entry.sourceCutoffSequence)},
        {QStringLiteral("createdAt"), entry.createdAt.toString(Qt::ISODateWithMs)}
    };
}

Result<DiaryEntry, DomainError> parseDiaryEnvelope(
    const QByteArray& plaintext,
    const StoredPrivateRecord& record) {
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(plaintext, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return Result<DiaryEntry, DomainError>::failure(
            domainError(QStringLiteral("PRIVATE_AUTH_FAILED"),
                        QStringLiteral("decrypted diary envelope is invalid")));
    }
    const QJsonObject object = document.object();
    DiaryEntry entry;
    entry.entryId = record.recordId;
    entry.profileId = record.profileId;
    entry.localDate = QDate::fromString(
        object.value(QStringLiteral("localDate")).toString(), Qt::ISODate);
    entry.body = object.value(QStringLiteral("body")).toString();
    entry.index = object.value(QStringLiteral("index")).toObject();
    entry.sourceCutoffSequence = static_cast<qint64>(
        object.value(QStringLiteral("sourceCutoffSequence")).toDouble(0));
    entry.keyVersion = record.encrypted.keyVersion;
    entry.createdAt = QDateTime::fromString(
        object.value(QStringLiteral("createdAt")).toString(), Qt::ISODateWithMs);
    if (!entry.localDate.isValid() || entry.body.trimmed().isEmpty()) {
        return Result<DiaryEntry, DomainError>::failure(
            domainError(QStringLiteral("PRIVATE_AUTH_FAILED"),
                        QStringLiteral("decrypted diary fields are invalid")));
    }
    return Result<DiaryEntry, DomainError>::success(entry);
}

QList<ChatMessage> diaryProjection(const DiaryRequest& request) {
    QJsonObject input;
    input.insert(QStringLiteral("localDate"), request.localDate.toString(Qt::ISODate));
    input.insert(QStringLiteral("events"), QJsonArray::fromStringList(
        request.eventSummaries.mid(0, 64)));
    input.insert(QStringLiteral("innerThoughtRefs"), QJsonArray::fromStringList(
        request.innerThoughtRefs.mid(0, 32)));
    input.insert(QStringLiteral("committedMemories"), QJsonArray::fromStringList(
        request.committedMemorySummaries.mid(0, 32)));
    input.insert(QStringLiteral("emotionTrajectoryAvailable"),
                 !request.emotionTrajectory.isEmpty());

    ChatMessage system;
    system.role = QStringLiteral("system");
    system.content = QStringLiteral(
        "写一篇自由但简短的睡前日记。只能依据输入事实；没有情绪轨迹时不要编造"
        "连续情绪经历。只返回 body 字符串和不含正文的 index 对象。");
    ChatMessage user;
    user.role = QStringLiteral("user");
    user.content = QString::fromUtf8(
        QJsonDocument(input).toJson(QJsonDocument::Compact));
    return {system, user};
}

} // namespace

DiaryService::DiaryService(
    QString profileId,
    ModelRouter* modelRouter,
    ContextAssembler* contextAssembler,
    PrivateKeyProvider* keyProvider,
    PrivatePsycheCrypto* crypto,
    SqlitePrivatePsycheRepository* repository,
    ModelRole selfReadRole,
    EventLedger* eventLedger)
    : m_profileId(std::move(profileId)),
      m_modelRouter(modelRouter),
      m_contextAssembler(contextAssembler),
      m_keyProvider(keyProvider),
      m_crypto(crypto),
      m_repository(repository),
      m_selfReadRole(selfReadRole),
      m_eventLedger(eventLedger) {}

DiaryService::~DiaryService() {
    m_alive->store(false, std::memory_order_release);
}

void DiaryService::composeAsync(
    const DiaryRequest& request,
    StagingSession& staging,
    const CancellationToken& token,
    DiaryHandler handler) {
    if (!handler) return;
    if (token.isCancelled()) {
        handler(Result<QString, DomainError>::failure(
            domainError(QStringLiteral("SLEEP_CANCELLED"),
                        QStringLiteral("diary request was cancelled"))));
        return;
    }
    if (request.profileId != m_profileId || request.sessionId != staging.sessionId
        || !request.localDate.isValid() || !m_modelRouter || !m_contextAssembler
        || !m_keyProvider || !m_crypto || !m_repository) {
        handler(Result<QString, DomainError>::failure(
            domainError(QStringLiteral("PRIVATE_STORE_UNAVAILABLE"),
                        QStringLiteral("diary dependencies are unavailable"))));
        return;
    }
    const auto existing = m_repository->committedDiaryForDate(
        m_profileId, request.localDate);
    if (!existing.isOk()) {
        handler(Result<QString, DomainError>::failure(existing.error()));
        return;
    }
    if (existing.value().has_value()) {
        handler(Result<QString, DomainError>::success(existing.value()->recordId));
        return;
    }
    const auto key = m_keyProvider->loadOrCreate(m_profileId);
    if (!key.isOk()) {
        handler(Result<QString, DomainError>::failure(key.error()));
        return;
    }

    ContextRequest contextRequest;
    contextRequest.queryBudgetChars = 16000;
    contextRequest.requestedPartitions = {ContextPartition::DiaryProjection};
    contextRequest.projections = {
        ContextProjection{ContextPartition::DiaryProjection,
                          diaryProjection(request)}};
    const auto messages = m_contextAssembler->assemble(
        ModelRole::Diary, contextRequest);
    if (!messages.isOk()) {
        handler(Result<QString, DomainError>::failure(messages.error()));
        return;
    }
    ModelRequest modelRequest;
    modelRequest.role = ModelRole::Diary;
    modelRequest.messages = messages.value();
    modelRequest.responseSchema = diarySchema();
    modelRequest.profileId = m_profileId;
    modelRequest.sessionId = request.sessionId;
    modelRequest.petName = request.petName;
    const PrivateKeyMaterial keyMaterial = key.value();
    const QString sessionId = staging.sessionId;
    const std::shared_ptr<std::atomic_bool> alive = m_alive;
    m_modelRouter->completeAsync(
        modelRequest,
        [this, alive, request, sessionId, token, keyMaterial,
         handler = std::move(handler)]
        (Result<ModelCompletion, DomainError> completion) mutable {
            if (!alive->load(std::memory_order_acquire)) return;
            if (token.isCancelled()) {
                handler(Result<QString, DomainError>::failure(
                    domainError(QStringLiteral("SLEEP_CANCELLED"),
                                QStringLiteral("late diary callback was discarded"))));
                return;
            }
            if (!completion.isOk()) {
                handler(Result<QString, DomainError>::failure(completion.error()));
                return;
            }
            const auto parsed = parseDiary(completion.value().response.content);
            if (!parsed.isOk()) {
                handler(Result<QString, DomainError>::failure(parsed.error()));
                return;
            }
            DiaryEntry entry;
            entry.entryId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            entry.profileId = m_profileId;
            entry.localDate = request.localDate;
            entry.body = parsed.value().first;
            entry.index = parsed.value().second;
            entry.sourceCutoffSequence = request.sourceCutoffSequence;
            entry.keyVersion = keyMaterial.keyVersion;
            entry.createdAt = QDateTime::currentDateTimeUtc();
            const PrivateRecordAad aad{
                1, m_profileId, QStringLiteral("diary_entry"), entry.entryId,
                keyMaterial.keyVersion};
            const auto encrypted = m_crypto->encrypt(
                QJsonDocument(diaryEnvelope(entry)).toJson(QJsonDocument::Compact),
                aad, keyMaterial);
            if (!encrypted.isOk()) {
                handler(Result<QString, DomainError>::failure(encrypted.error()));
                return;
            }
            const auto staged = m_repository->stageDiary(
                sessionId, entry, encrypted.value());
            if (!staged.isOk()) {
                handler(Result<QString, DomainError>::failure(staged.error()));
                return;
            }
            handler(Result<QString, DomainError>::success(entry.entryId));
        });
}

Result<DiaryEntry, DomainError> DiaryService::readForSelf(
    const QString& entryId) const {
    if (m_selfReadRole != ModelRole::Diary) {
        return Result<DiaryEntry, DomainError>::failure(
            domainError(QStringLiteral("CONTEXT_SCOPE_DENIED"),
                        QStringLiteral("model role cannot read private diary")));
    }
    return readEntry(entryId);
}

Result<DiaryEntry, DomainError> DiaryService::readForOwner(
    const QString& entryId,
    const OwnerAuthContext& auth) const {
    if (!auth.authenticated || auth.profileId != m_profileId) {
        return Result<DiaryEntry, DomainError>::failure(
            domainError(QStringLiteral("OWNER_AUTH_FAILED"),
                        QStringLiteral("owner diary authorization failed")));
    }
    return readEntry(entryId);
}

Result<bool, DomainError> DiaryService::hasCommittedDiaryForDate(
    const QDate& localDate) const {
    if (!m_repository || !localDate.isValid()) {
        return Result<bool, DomainError>::failure(
            domainError(QStringLiteral("PRIVATE_STORE_UNAVAILABLE"),
                        QStringLiteral("diary status is unavailable")));
    }
    const auto diary = m_repository->committedDiaryForDate(m_profileId, localDate);
    if (!diary.isOk()) {
        return Result<bool, DomainError>::failure(diary.error());
    }
    return Result<bool, DomainError>::success(diary.value().has_value());
}

Result<DiaryEntry, DomainError> DiaryService::readEntry(
    const QString& entryId) const {
    if (!m_repository || !m_keyProvider || !m_crypto) {
        return Result<DiaryEntry, DomainError>::failure(
            domainError(QStringLiteral("PRIVATE_STORE_UNAVAILABLE"),
                        QStringLiteral("diary dependencies are unavailable")));
    }
    const auto stored = m_repository->diary(entryId);
    if (!stored.isOk()) return Result<DiaryEntry, DomainError>::failure(stored.error());
    if (!stored.value().has_value()) {
        return Result<DiaryEntry, DomainError>::failure(
            domainError(QStringLiteral("CONTEXT_SCOPE_DENIED"),
                        QStringLiteral("diary entry is unavailable")));
    }
    const StoredPrivateRecord& record = *stored.value();
    if (record.profileId != m_profileId) {
        return Result<DiaryEntry, DomainError>::failure(
            domainError(QStringLiteral("PRIVATE_AUTH_FAILED"),
                        QStringLiteral("diary AAD profile does not match")));
    }
    const auto key = m_keyProvider->loadOrCreate(m_profileId);
    if (!key.isOk()) return Result<DiaryEntry, DomainError>::failure(key.error());
    const PrivateRecordAad aad{
        record.encrypted.schemaVersion, record.profileId, record.recordType,
        record.recordId, record.encrypted.keyVersion};
    const auto plaintext = m_crypto->decrypt(record.encrypted, aad, key.value());
    if (!plaintext.isOk()) {
        return Result<DiaryEntry, DomainError>::failure(plaintext.error());
    }
    return parseDiaryEnvelope(plaintext.value(), record);
}

Result<void, DomainError> DiaryService::finalizeSession(
    const QString& sessionId) {
    if (!m_repository || !m_keyProvider || !m_crypto) {
        return Result<void, DomainError>::failure(
            domainError(QStringLiteral("PRIVATE_STORE_UNAVAILABLE"),
                        QStringLiteral("diary finalize dependencies are unavailable")));
    }
    const auto key = m_keyProvider->loadOrCreate(m_profileId);
    if (!key.isOk()) return Result<void, DomainError>::failure(key.error());
    const auto records = m_repository->preparedDiaries(sessionId, m_profileId);
    if (!records.isOk()) return Result<void, DomainError>::failure(records.error());
    for (const StoredPrivateRecord& record : records.value()) {
        const PrivateRecordAad aad{
            record.encrypted.schemaVersion, record.profileId, record.recordType,
            record.recordId, record.encrypted.keyVersion};
        const auto plaintext = m_crypto->decrypt(record.encrypted, aad, key.value());
        if (!plaintext.isOk()) return Result<void, DomainError>::failure(plaintext.error());
        const auto entry = parseDiaryEnvelope(plaintext.value(), record);
        if (!entry.isOk()) return Result<void, DomainError>::failure(entry.error());
        const auto finalized = m_repository->finalizeDiary(
            sessionId, entry.value(), record.encrypted);
        if (!finalized.isOk()) {
            return Result<void, DomainError>::failure(finalized.error());
        }
        if (m_eventLedger) {
            EventDraft event;
            event.eventId = finalized.value();
            event.profileId = m_profileId;
            event.type = QStringLiteral("DiaryStored");
            event.source = QStringLiteral("DiaryService");
            event.sessionId = sessionId;
            event.privacy = EventPrivacy::Private;
            event.privateReference = PrivateEventReference{
                QStringLiteral("private_psyche"), QStringLiteral("diary_entry"),
                finalized.value(), m_profileId};
            m_eventLedger->append(event);
        }
    }
    return Result<void, DomainError>::success();
}

Result<void, DomainError> DiaryService::abortSession(
    const QString& sessionId) {
    if (!m_repository) return Result<void, DomainError>::success();
    return m_repository->abortSession(sessionId);
}
