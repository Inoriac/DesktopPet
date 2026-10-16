#pragma once
#include "ai/ai_tool.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <functional>
#include "ai/identity/identity_types.h"

struct TaskBudget {
    int maxToolCalls = 12;
    int maxReplans = 2;
    qint64 maxActiveMs = 300000;
    int modelTimeoutMs = 60000;
    int toolTimeoutMs = 30000;
};

struct TaskBudgetExtension {
    int toolCalls = 0;
    int replans = 0;
    qint64 activeMs = 0;
};

struct TaskStartRequest {
    QStringList requiredSourceTools;
    QString profileId;
    QString subjectId;
    OriginKind originKind = OriginKind::Internal;
    QString goal;
    qint64 personalityVersion = 0;
    QJsonObject personalitySnapshot;
    QJsonObject relationshipSnapshot;
    QJsonObject emotionSnapshot;
    TaskBudget budget;
};

struct TaskRecord {
    QString id;
    QString profileId;
    QString subjectId;
    OriginKind originKind = OriginKind::Internal;
    QString goal;
    QString status = "Received";
    QString plan;
    QString result;
    QString reason;
    QString completionCriteria;
    QStringList requiredSourceTools;
    qint64 personalityVersion = 0;
    QJsonObject personalitySnapshot;
    QJsonObject relationshipSnapshot;
    QJsonObject initialEmotion;
    QJsonObject currentEmotion;
    QJsonArray steps;
    QJsonArray decisions;
    QJsonArray evidence;
    QJsonArray checkpoints;
    QJsonArray handledTriggers;
    QString pendingTrigger;
    QString userFeedback;
    QString requirementUpdates;
    QString createdAt;
    QString updatedAt;
    TaskBudget budget;
    int toolCalls = 0;
    int replans = 0;
    int consecutiveFailures = 0;
    qint64 activeMs = 0;
    qint64 lastAgencyActiveMs = -60000;
    bool accepted = false;
    bool needsRevalidation = false;
    bool terminal() const;
    QJsonObject toJson() const;
    static TaskRecord fromJson(const QJsonObject& json);
};

struct TaskModelRequest {
    QString requestId;
    QString role;
    QString phase;
    QString systemPrompt;
    QJsonObject context;
    int timeoutMs = 0;
};
struct TaskModelReply {
    bool success = false;
    QJsonObject output;
    QString error;
};
struct TaskToolCall {
    QString id;
    QString name;
    QJsonObject arguments;
    bool readOnly = true;
    int timeoutMs = 0;
};
struct TaskToolReply {
    bool success = false;
    QJsonObject output;
    QString error;
    // A failed side effect is unknown unless the runtime can prove it did not occur.
    bool outcomeKnown = false;
    bool businessEvidence = true;
    QString stage = "completed";
};
struct TaskDependencies {
    using ModelCompletion = std::function<void(TaskModelReply)>;
    using ToolCompletion = std::function<void(TaskToolReply)>;
    std::function<void(const TaskModelRequest&, ModelCompletion)> requestModel;
    std::function<void(const TaskToolCall&, ToolCompletion)> executeTool;
    std::function<void(const QString&)> cancelModel;
    std::function<void(const QString&)> cancelTool;
    // Host metadata and policy are independent of model proposals.
    std::function<HostToolMetadata(const QString&)> toolMetadata;
    std::function<bool(const QString&, const QJsonObject&)> toolReadOnly;
    std::function<bool(const QString&, const QJsonObject&)> toolAllowed;
    std::function<QJsonArray()> availableTools;
    // Only relationship/emotion keys are refreshed; task personality remains immutable.
    std::function<QJsonObject()> refreshContext;
};
