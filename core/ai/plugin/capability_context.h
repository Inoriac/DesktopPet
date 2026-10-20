#pragma once

#include "ai_types.h"
#include <QHash>

struct CapabilityToolProjection {
    QJsonArray tools;
    QHash<QString, QJsonObject> operationRefs;
    QHash<QString, QJsonObject> definitionBindings;

    // Accept only names offered in this request. Preserve IDs and native calls.
    bool normalizeCall(LlmToolCall& call, QJsonObject* definitionBinding = nullptr) const;
};

// Request-only views. Archived messages and task steps remain complete.
class CapabilityContext {
public:
    static bool isMetadataTool(const QString& name);
    static QList<ChatMessage> projectMessages(const QList<ChatMessage>& messages);
    static QJsonArray projectSteps(const QJsonArray& steps);
    static bool hasOperation(const QList<ChatMessage>& projected, const QJsonObject& ref);
    static bool hasOperation(const QJsonArray& projectedSteps, const QJsonObject& ref);
    static CapabilityToolProjection projectTools(const QJsonArray& baseTools,
                                                 const QList<ChatMessage>& projected);
    static CapabilityToolProjection projectTools(const QJsonArray& baseTools,
                                                 const QJsonArray& projectedSteps);
};
