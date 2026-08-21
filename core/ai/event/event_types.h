#ifndef DESKTOP_PET_EVENT_TYPES_H
#define DESKTOP_PET_EVENT_TYPES_H

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QString>

#include <optional>

#include "ai/domain/domain_result.h"

class AgentRuntimeServices;

enum class EventPrivacy {
    Normal,
    Sensitive,
    Private
};

struct PrivateEventReference {
    QString store;
    QString recordType;
    QString recordId;
    QString profileId;
};

struct EventDraft {
    QString eventId;
    int schemaVersion = 1;
    QString profileId;
    QString type;
    QString source;
    QString sessionId;
    QJsonObject payload;
    std::optional<PrivateEventReference> privateReference;
    EventPrivacy privacy = EventPrivacy::Normal;
    QDateTime occurredAt;
};

struct EventRecord : EventDraft {
    qint64 sequence = 0;
    QDateTime createdAt;
};

class EventReadAuthorization {
public:
    const QString& consumerId() const { return m_consumerId; }
    const QString& profileId() const { return m_profileId; }
    bool allowsSensitivePayload() const { return m_allowSensitivePayload; }
    bool allowsPrivateReference(const QString& recordType) const;

private:
    friend class AgentRuntimeServices;
    EventReadAuthorization(QString consumerId,
                           QString profileId,
                           bool allowSensitivePayload,
                           QSet<QString> allowedPrivateRecordTypes);

    QString m_consumerId;
    QString m_profileId;
    bool m_allowSensitivePayload = false;
    QSet<QString> m_allowedPrivateRecordTypes;
};

struct EventFilter {
    QSet<QString> types;
    QString sessionId;
    EventReadAuthorization authorization;
};

enum class EventFieldType {
    String,
    Boolean,
    Number,
    Object,
    Array
};

struct EventSchemaDefinition {
    QString type;
    int version = 1;
    QHash<QString, EventFieldType> requiredFields;
    QHash<QString, EventFieldType> optionalFields;
    bool preserveUnknownFields = true;
};

QString eventPrivacyToString(EventPrivacy privacy);
std::optional<EventPrivacy> eventPrivacyFromString(const QString& value);
bool isCanonicalEventUuid(const QString& value);

QJsonObject privateEventReferenceToJson(const PrivateEventReference& reference);
Result<PrivateEventReference, DomainError> privateEventReferenceFromJson(
    const QJsonObject& object);
QJsonObject eventDraftToJson(const EventDraft& draft);
Result<EventDraft, DomainError> eventDraftFromJson(const QJsonObject& object);

#endif // DESKTOP_PET_EVENT_TYPES_H
