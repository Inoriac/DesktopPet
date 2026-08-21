#include "event_types.h"

#include <QJsonDocument>
#include <QUuid>

#include <utility>

EventReadAuthorization::EventReadAuthorization(
    QString consumerId,
    QString profileId,
    bool allowSensitivePayload,
    QSet<QString> allowedPrivateRecordTypes)
    : m_consumerId(std::move(consumerId))
    , m_profileId(std::move(profileId))
    , m_allowSensitivePayload(allowSensitivePayload)
    , m_allowedPrivateRecordTypes(std::move(allowedPrivateRecordTypes)) {}

bool EventReadAuthorization::allowsPrivateReference(const QString& recordType) const {
    return m_allowedPrivateRecordTypes.contains(recordType);
}

QString eventPrivacyToString(EventPrivacy privacy) {
    switch (privacy) {
    case EventPrivacy::Normal: return QStringLiteral("normal");
    case EventPrivacy::Sensitive: return QStringLiteral("sensitive");
    case EventPrivacy::Private: return QStringLiteral("private");
    }
    return QStringLiteral("normal");
}

std::optional<EventPrivacy> eventPrivacyFromString(const QString& value) {
    if (value == QLatin1String("normal")) return EventPrivacy::Normal;
    if (value == QLatin1String("sensitive")) return EventPrivacy::Sensitive;
    if (value == QLatin1String("private")) return EventPrivacy::Private;
    return std::nullopt;
}

bool isCanonicalEventUuid(const QString& value) {
    const QString candidate = value.trimmed();
    const QUuid uuid = QUuid::fromString(candidate);
    return !uuid.isNull()
        && uuid.toString(QUuid::WithoutBraces).toLower() == candidate;
}

QJsonObject privateEventReferenceToJson(const PrivateEventReference& reference) {
    return {
        {QStringLiteral("store"), reference.store},
        {QStringLiteral("recordType"), reference.recordType},
        {QStringLiteral("recordId"), reference.recordId},
        {QStringLiteral("profileId"), reference.profileId}
    };
}

Result<PrivateEventReference, DomainError> privateEventReferenceFromJson(
    const QJsonObject& object) {
    PrivateEventReference reference;
    reference.store = object.value(QStringLiteral("store")).toString();
    reference.recordType = object.value(QStringLiteral("recordType")).toString();
    reference.recordId = object.value(QStringLiteral("recordId")).toString();
    reference.profileId = object.value(QStringLiteral("profileId")).toString();
    if (reference.store.isEmpty() || reference.recordType.isEmpty()
        || reference.recordId.isEmpty() || reference.profileId.isEmpty()) {
        return Result<PrivateEventReference, DomainError>::failure(
            domainError(QStringLiteral("EVT_SCHEMA_INVALID"),
                        QStringLiteral("private event reference is incomplete")));
    }
    return Result<PrivateEventReference, DomainError>::success(std::move(reference));
}

QJsonObject eventDraftToJson(const EventDraft& draft) {
    QJsonObject object;
    object.insert(QStringLiteral("eventId"), draft.eventId);
    object.insert(QStringLiteral("schemaVersion"), draft.schemaVersion);
    object.insert(QStringLiteral("profileId"), draft.profileId);
    object.insert(QStringLiteral("type"), draft.type);
    object.insert(QStringLiteral("source"), draft.source);
    object.insert(QStringLiteral("sessionId"), draft.sessionId);
    object.insert(QStringLiteral("payload"), draft.payload);
    object.insert(QStringLiteral("privacy"), eventPrivacyToString(draft.privacy));
    object.insert(QStringLiteral("occurredAt"),
                  draft.occurredAt.toUTC().toString(Qt::ISODateWithMs));
    if (draft.privateReference.has_value()) {
        object.insert(QStringLiteral("privateReference"),
                      privateEventReferenceToJson(*draft.privateReference));
    }
    return object;
}

Result<EventDraft, DomainError> eventDraftFromJson(const QJsonObject& object) {
    EventDraft draft;
    draft.eventId = object.value(QStringLiteral("eventId")).toString();
    draft.schemaVersion = object.value(QStringLiteral("schemaVersion")).toInt();
    draft.profileId = object.value(QStringLiteral("profileId")).toString();
    draft.type = object.value(QStringLiteral("type")).toString();
    draft.source = object.value(QStringLiteral("source")).toString();
    draft.sessionId = object.value(QStringLiteral("sessionId")).toString();
    if (!object.value(QStringLiteral("payload")).isObject()) {
        return Result<EventDraft, DomainError>::failure(
            domainError(QStringLiteral("EVT_SCHEMA_INVALID"),
                        QStringLiteral("event payload is not an object")));
    }
    draft.payload = object.value(QStringLiteral("payload")).toObject();
    const std::optional<EventPrivacy> privacy =
        eventPrivacyFromString(object.value(QStringLiteral("privacy")).toString());
    if (!privacy.has_value()) {
        return Result<EventDraft, DomainError>::failure(
            domainError(QStringLiteral("EVT_SCHEMA_INVALID"),
                        QStringLiteral("event privacy is invalid")));
    }
    draft.privacy = *privacy;
    draft.occurredAt = QDateTime::fromString(
        object.value(QStringLiteral("occurredAt")).toString(), Qt::ISODateWithMs);
    if (object.contains(QStringLiteral("privateReference"))) {
        if (!object.value(QStringLiteral("privateReference")).isObject()) {
            return Result<EventDraft, DomainError>::failure(
                domainError(QStringLiteral("EVT_SCHEMA_INVALID"),
                            QStringLiteral("private event reference is not an object")));
        }
        auto parsed = privateEventReferenceFromJson(
            object.value(QStringLiteral("privateReference")).toObject());
        if (!parsed.isOk()) return Result<EventDraft, DomainError>::failure(parsed.error());
        draft.privateReference = parsed.takeValue();
    }
    return Result<EventDraft, DomainError>::success(std::move(draft));
}
