#pragma once

#include "ai_types.h"

// Request-only views. Archived messages and task steps remain complete.
class CapabilityContext {
public:
    static bool isMetadataTool(const QString& name);
    static QList<ChatMessage> projectMessages(const QList<ChatMessage>& messages);
    static QJsonArray projectSteps(const QJsonArray& steps);
    static bool hasOperation(const QList<ChatMessage>& projected, const QJsonObject& ref);
    static bool hasOperation(const QJsonArray& projectedSteps, const QJsonObject& ref);
};
