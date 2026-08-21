#include "event_schema_registry.h"

#include <QJsonArray>
#include <QJsonValue>

namespace {

bool matchesType(const QJsonValue& value, EventFieldType type) {
    switch (type) {
    case EventFieldType::String: return value.isString();
    case EventFieldType::Boolean: return value.isBool();
    case EventFieldType::Number: return value.isDouble();
    case EventFieldType::Object: return value.isObject();
    case EventFieldType::Array: return value.isArray();
    }
    return false;
}

Result<void, DomainError> invalid(const QString& message) {
    return Result<void, DomainError>::failure(
        domainError(QStringLiteral("EVT_SCHEMA_INVALID"), message));
}

} // namespace

QString EventSchemaRegistry::key(const QString& type, int version) {
    return type + QLatin1Char('#') + QString::number(version);
}

Result<void, DomainError> EventSchemaRegistry::registerSchema(
    const EventSchemaDefinition& schema) {
    if (m_frozen) return invalid(QStringLiteral("event schema registry is frozen"));
    if (schema.type.trimmed().isEmpty() || schema.version <= 0) {
        return invalid(QStringLiteral("event schema identity is invalid"));
    }
    const QString schemaKey = key(schema.type, schema.version);
    if (m_schemas.contains(schemaKey)) {
        return invalid(QStringLiteral("event schema is already registered"));
    }
    for (auto it = schema.requiredFields.cbegin(); it != schema.requiredFields.cend(); ++it) {
        if (it.key().isEmpty() || schema.optionalFields.contains(it.key())) {
            return invalid(QStringLiteral("event schema fields overlap or are empty"));
        }
    }
    m_schemas.insert(schemaKey, schema);
    return Result<void, DomainError>::success();
}

Result<void, DomainError> EventSchemaRegistry::validate(const EventDraft& draft) const {
    if (!isCanonicalEventUuid(draft.profileId)
        || draft.type.trimmed().isEmpty()
        || draft.source.trimmed().isEmpty()
        || draft.schemaVersion <= 0
        || (!draft.eventId.isEmpty() && !isCanonicalEventUuid(draft.eventId))) {
        return invalid(QStringLiteral("event envelope is invalid"));
    }
    const auto schemaIt = m_schemas.constFind(key(draft.type, draft.schemaVersion));
    if (schemaIt == m_schemas.constEnd()) {
        return invalid(QStringLiteral("event schema is not registered"));
    }

    if (draft.privacy == EventPrivacy::Private) {
        if (!draft.payload.isEmpty() || !draft.privateReference.has_value()) {
            return invalid(QStringLiteral("private event must contain only a reference"));
        }
        const PrivateEventReference& reference = *draft.privateReference;
        if (reference.store != QLatin1String("private_psyche")
            || (reference.recordType != QLatin1String("inner_thought")
                && reference.recordType != QLatin1String("diary_entry"))
            || !isCanonicalEventUuid(reference.recordId)
            || !isCanonicalEventUuid(reference.profileId)
            || reference.profileId != draft.profileId) {
            return invalid(QStringLiteral("private event reference is invalid"));
        }
        return Result<void, DomainError>::success();
    }

    if (draft.privateReference.has_value()) {
        return invalid(QStringLiteral("non-private event cannot contain a private reference"));
    }

    const EventSchemaDefinition& schema = schemaIt.value();
    for (auto it = schema.requiredFields.cbegin(); it != schema.requiredFields.cend(); ++it) {
        if (!draft.payload.contains(it.key())
            || !matchesType(draft.payload.value(it.key()), it.value())) {
            return invalid(QStringLiteral("required event field is missing or has wrong type"));
        }
    }
    for (auto it = schema.optionalFields.cbegin(); it != schema.optionalFields.cend(); ++it) {
        if (draft.payload.contains(it.key())
            && !matchesType(draft.payload.value(it.key()), it.value())) {
            return invalid(QStringLiteral("optional event field has wrong type"));
        }
    }
    if (!schema.preserveUnknownFields) {
        for (auto it = draft.payload.cbegin(); it != draft.payload.cend(); ++it) {
            if (!schema.requiredFields.contains(it.key())
                && !schema.optionalFields.contains(it.key())) {
                return invalid(QStringLiteral("event payload contains an unknown field"));
            }
        }
    }
    return Result<void, DomainError>::success();
}

Result<void, DomainError> registerBuiltInEventSchemas(EventSchemaRegistry& registry) {
    QList<EventSchemaDefinition> definitions;

    EventSchemaDefinition user;
    user.type = QStringLiteral("UserMessageReceived");
    user.requiredFields = {
        {QStringLiteral("text"), EventFieldType::String},
        {QStringLiteral("triggerTag"), EventFieldType::String}
    };
    user.optionalFields = {{QStringLiteral("inputMethod"), EventFieldType::String}};
    definitions.append(user);

    EventSchemaDefinition assistant;
    assistant.type = QStringLiteral("AssistantResponseProduced");
    assistant.requiredFields = {
        {QStringLiteral("text"), EventFieldType::String},
        {QStringLiteral("triggerTag"), EventFieldType::String}
    };
    assistant.optionalFields = {{QStringLiteral("modelRole"), EventFieldType::String}};
    definitions.append(assistant);

    EventSchemaDefinition model;
    model.type = QStringLiteral("ModelCallCompleted");
    model.requiredFields = {
        {QStringLiteral("role"), EventFieldType::String},
        {QStringLiteral("success"), EventFieldType::Boolean},
        {QStringLiteral("durationMs"), EventFieldType::Number}
    };
    model.optionalFields = {
        {QStringLiteral("provider"), EventFieldType::String},
        {QStringLiteral("model"), EventFieldType::String},
        {QStringLiteral("errorCode"), EventFieldType::String}
    };
    definitions.append(model);

    EventSchemaDefinition tool;
    tool.type = QStringLiteral("ToolExecutionCompleted");
    tool.requiredFields = {
        {QStringLiteral("toolName"), EventFieldType::String},
        {QStringLiteral("success"), EventFieldType::Boolean}
    };
    tool.optionalFields = {
        {QStringLiteral("resultSummary"), EventFieldType::String},
        {QStringLiteral("errorCode"), EventFieldType::String}
    };
    definitions.append(tool);

    EventSchemaDefinition degraded;
    degraded.type = QStringLiteral("RuntimeDegraded");
    degraded.requiredFields = {
        {QStringLiteral("capability"), EventFieldType::String},
        {QStringLiteral("reasonCode"), EventFieldType::String}
    };
    degraded.optionalFields = {
        {QStringLiteral("diagnosticHash"), EventFieldType::String}
    };
    definitions.append(degraded);

    for (const EventSchemaDefinition& definition : definitions) {
        Result<void, DomainError> result = registry.registerSchema(definition);
        if (!result.isOk()) return result;
    }
    return Result<void, DomainError>::success();
}
