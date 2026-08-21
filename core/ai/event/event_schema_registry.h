#ifndef DESKTOP_PET_EVENT_SCHEMA_REGISTRY_H
#define DESKTOP_PET_EVENT_SCHEMA_REGISTRY_H

#include <QHash>

#include "event_types.h"

class EventSchemaRegistry {
public:
    Result<void, DomainError> registerSchema(const EventSchemaDefinition& schema);
    Result<void, DomainError> validate(const EventDraft& draft) const;
    void freeze() { m_frozen = true; }
    bool isFrozen() const { return m_frozen; }

private:
    static QString key(const QString& type, int version);

    QHash<QString, EventSchemaDefinition> m_schemas;
    bool m_frozen = false;
};

Result<void, DomainError> registerBuiltInEventSchemas(EventSchemaRegistry& registry);

#endif // DESKTOP_PET_EVENT_SCHEMA_REGISTRY_H
