#include "inner_thought_service.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QUuid>

#include <utility>

#include "ai/event/event_ledger.h"
#include "ai/model/model_router.h"
#include "private_key_provider.h"
#include "private_psyche_crypto.h"
#include "sqlite_private_psyche_repository.h"

namespace {

QJsonObject innerThoughtSchema() {
    const QJsonObject stringType{{QStringLiteral("type"), QStringLiteral("string")}};
    const QJsonObject boolType{{QStringLiteral("type"), QStringLiteral("boolean")}};
    return {
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("required"), QJsonArray{
             QStringLiteral("appraisal"), QStringLiteral("desire"),
             QStringLiteral("uncertainty"), QStringLiteral("unresolved")}},
        {QStringLiteral("properties"), QJsonObject{
             {QStringLiteral("appraisal"), stringType},
             {QStringLiteral("desire"), stringType},
             {QStringLiteral("uncertainty"), stringType},
             {QStringLiteral("unresolved"), boolType}}}
    };
}

Result<InnerThoughtSummary, DomainError> parseSummary(
    const QString& content,
    const QString& profileId,
    const QString& sourceEventId,
    const QString& entryId,
    const QDateTime& createdAt) {
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(content.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return Result<InnerThoughtSummary, DomainError>::failure(
            domainError(QStringLiteral("MODEL_OUTPUT_INVALID"),
                        QStringLiteral("inner thought output is not a JSON object")));
    }
    const QJsonObject object = document.object();
    InnerThoughtSummary summary;
    summary.entryId = entryId;
    summary.profileId = profileId;
    summary.sourceEventId = sourceEventId;
    summary.appraisal = object.value(QStringLiteral("appraisal"))
                            .toString().simplified().left(512);
    summary.desire = object.value(QStringLiteral("desire"))
                         .toString().simplified().left(512);
    summary.uncertainty = object.value(QStringLiteral("uncertainty"))
                             .toString().simplified().left(512);
    summary.unresolved = object.value(QStringLiteral("unresolved")).toBool(false);
    summary.createdAt = createdAt;
    if (summary.appraisal.isEmpty() && summary.desire.isEmpty()
        && summary.uncertainty.isEmpty()) {
        return Result<InnerThoughtSummary, DomainError>::failure(
            domainError(QStringLiteral("MODEL_OUTPUT_INVALID"),
                        QStringLiteral("inner thought summary is empty")));
    }
    return Result<InnerThoughtSummary, DomainError>::success(summary);
}

QJsonObject summaryPayload(const InnerThoughtSummary& summary) {
    return {
        {QStringLiteral("appraisal"), summary.appraisal},
        {QStringLiteral("desire"), summary.desire},
        {QStringLiteral("uncertainty"), summary.uncertainty},
        {QStringLiteral("unresolved"), summary.unresolved}
    };
}

} // namespace

InnerThoughtService::InnerThoughtService(
    QString profileId,
    ModelRouter* modelRouter,
    PrivateKeyProvider* keyProvider,
    PrivatePsycheCrypto* crypto,
    SqlitePrivatePsycheRepository* repository,
    EventLedger* eventLedger)
    : m_profileId(std::move(profileId)),
      m_modelRouter(modelRouter),
      m_keyProvider(keyProvider),
      m_crypto(crypto),
      m_repository(repository),
      m_eventLedger(eventLedger) {}

InnerThoughtService::~InnerThoughtService() {
    m_alive->store(false, std::memory_order_release);
}

void InnerThoughtService::createAsync(
    const InnerThoughtRequest& request,
    const CancellationToken& token,
    InnerThoughtHandler handler) {
    if (!handler) return;
    if (token.isCancelled()) {
        handler(Result<QString, DomainError>::failure(
            domainError(QStringLiteral("SLEEP_CANCELLED"),
                        QStringLiteral("inner thought request was cancelled"))));
        return;
    }
    if (request.profileId != m_profileId
        || request.privacy != EventPrivacy::Private
        || !m_modelRouter || !m_keyProvider || !m_crypto || !m_repository) {
        handler(Result<QString, DomainError>::failure(
            domainError(QStringLiteral("PRIVATE_STORE_UNAVAILABLE"),
                        QStringLiteral("inner thought dependencies are unavailable"))));
        return;
    }
    const auto key = m_keyProvider->loadOrCreate(m_profileId);
    if (!key.isOk()) {
        handler(Result<QString, DomainError>::failure(key.error()));
        return;
    }

    ChatMessage system;
    system.role = QStringLiteral("system");
    system.content = QStringLiteral(
        "根据给定事件生成简短心理摘要。只返回 appraisal、desire、uncertainty、"
        "unresolved 四个 JSON 字段；不要输出或复述推理过程。");
    ChatMessage user;
    user.role = QStringLiteral("user");
    user.content = QString::fromUtf8(
        QJsonDocument(request.contextSnapshot).toJson(QJsonDocument::Compact));

    ModelRequest modelRequest;
    modelRequest.role = ModelRole::FastExtract;
    modelRequest.messages = {system, user};
    modelRequest.responseSchema = innerThoughtSchema();
    modelRequest.profileId = m_profileId;
    const PrivateKeyMaterial keyMaterial = key.value();
    const std::shared_ptr<std::atomic_bool> alive = m_alive;
    m_modelRouter->completeAsync(
        modelRequest,
        [this, alive, request, token, keyMaterial, handler = std::move(handler)]
        (Result<ModelCompletion, DomainError> completion) mutable {
            if (!alive->load(std::memory_order_acquire)) return;
            if (token.isCancelled()) {
                handler(Result<QString, DomainError>::failure(
                    domainError(QStringLiteral("SLEEP_CANCELLED"),
                                QStringLiteral("late inner thought callback was discarded"))));
                return;
            }
            if (!completion.isOk()) {
                handler(Result<QString, DomainError>::failure(completion.error()));
                return;
            }
            const QString entryId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            const QDateTime now = QDateTime::currentDateTimeUtc();
            const auto parsed = parseSummary(
                completion.value().response.content, m_profileId,
                request.sourceEventId, entryId, now);
            if (!parsed.isOk()) {
                handler(Result<QString, DomainError>::failure(parsed.error()));
                return;
            }
            const QJsonObject payload = summaryPayload(parsed.value());
            const PrivateRecordAad aad{
                1, m_profileId, QStringLiteral("inner_thought"), entryId,
                keyMaterial.keyVersion};
            const auto encrypted = m_crypto->encrypt(
                QJsonDocument(payload).toJson(QJsonDocument::Compact), aad, keyMaterial);
            if (!encrypted.isOk()) {
                handler(Result<QString, DomainError>::failure(encrypted.error()));
                return;
            }
            const QJsonObject index{
                {QStringLiteral("unresolved"), parsed.value().unresolved},
                {QStringLiteral("sourceEventId"), request.sourceEventId}
            };
            const auto stored = m_repository->saveInnerThought(
                parsed.value(), encrypted.value(), index);
            if (!stored.isOk()) {
                handler(Result<QString, DomainError>::failure(stored.error()));
                return;
            }
            if (m_eventLedger) {
                EventDraft event;
                event.eventId = entryId;
                event.profileId = m_profileId;
                event.type = QStringLiteral("InnerThoughtStored");
                event.source = QStringLiteral("InnerThoughtService");
                event.privacy = EventPrivacy::Private;
                event.privateReference = PrivateEventReference{
                    QStringLiteral("private_psyche"),
                    QStringLiteral("inner_thought"), entryId, m_profileId};
                m_eventLedger->append(event);
            }
            handler(Result<QString, DomainError>::success(entryId));
        });
}

Result<InnerThoughtSummary, DomainError> InnerThoughtService::readSummary(
    const QString& entryId) const {
    if (!m_repository || !m_keyProvider || !m_crypto) {
        return Result<InnerThoughtSummary, DomainError>::failure(
            domainError(QStringLiteral("PRIVATE_STORE_UNAVAILABLE"),
                        QStringLiteral("inner thought dependencies are unavailable")));
    }
    const auto stored = m_repository->innerThought(entryId);
    if (!stored.isOk()) {
        return Result<InnerThoughtSummary, DomainError>::failure(stored.error());
    }
    if (!stored.value().has_value() || stored.value()->profileId != m_profileId) {
        return Result<InnerThoughtSummary, DomainError>::failure(
            domainError(QStringLiteral("CONTEXT_SCOPE_DENIED"),
                        QStringLiteral("inner thought is unavailable")));
    }
    const auto key = m_keyProvider->loadOrCreate(m_profileId);
    if (!key.isOk()) {
        return Result<InnerThoughtSummary, DomainError>::failure(key.error());
    }
    const StoredPrivateRecord& record = *stored.value();
    const PrivateRecordAad aad{
        record.encrypted.schemaVersion, record.profileId, record.recordType,
        record.recordId, record.encrypted.keyVersion};
    const auto plaintext = m_crypto->decrypt(record.encrypted, aad, key.value());
    if (!plaintext.isOk()) {
        return Result<InnerThoughtSummary, DomainError>::failure(plaintext.error());
    }
    const auto parsed = parseSummary(
        QString::fromUtf8(plaintext.value()), record.profileId,
        record.sourceEventId, record.recordId, record.createdAt);
    return parsed;
}
