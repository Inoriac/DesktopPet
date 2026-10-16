#pragma once
#include "ai/domain/domain_result.h"
#include "ai/context/context_spec.h"
#include <QJsonArray>
#include <functional>
#include "ai/ai_tool.h"

struct PluginLocation { QString rootPath; };
struct CapabilityRef {
    QString pluginId, entryId, entryVersion, operationId;
    bool operator==(const CapabilityRef&) const = default;
    QJsonObject toJson() const;
    static CapabilityRef fromJson(const QJsonObject& value);
    QString key() const { return pluginId + '/' + entryId + '/' + entryVersion + '/' + operationId; }
};
struct PluginRegistration {
    QString pluginId;
    QJsonArray entries;
    QStringList diagnostics;
    quint64 catalogRevision = 0;
};
struct LoadedCapability {
    CapabilityRef ref;
    QString kind, instructions, definitionDigest, rootPath, definitionPath;
    quint64 catalogRevision = 0;
    QJsonObject operation;
    QJsonArray configuration;
    QJsonObject hostConfiguration; // private binding snapshot, never part of public JSON
    QJsonArray operations, dependencies;
    QJsonObject parameterSchema;
    QJsonObject toPublicJson() const;
};
struct PluginStateChange { QString pluginId; bool enabled = false; quint64 expectedRevision = 0; };
struct CapabilityCall {
    QString callId, activityId, grantRef;
    ContextSpec context;
    CapabilityRef ref;
    QJsonObject arguments;
    quint64 catalogRevision = 0;
    QString definitionDigest;
    int timeoutMs = 30000;
    bool autonomous = false;
    std::shared_ptr<ToolDispatchToken> dispatchToken;
};
struct CapabilityOutcome {
    ContextSpec context;
    quint64 catalogRevision=0;
    QString callId, activityId, stage = "failed", effectCertainty = "no_effect";
    QJsonValue data, receipt, tracking;
    DomainError error;
    bool executed = false;
    QJsonObject toJson() const;
};
using CapabilityCompletion = std::function<void(CapabilityOutcome)>;
