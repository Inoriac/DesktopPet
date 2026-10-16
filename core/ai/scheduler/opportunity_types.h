#pragma once
#include "ai/domain/domain_result.h"
#include "ai_types.h"
#include <QDateTime>
#include <QJsonObject>
#include <QStringList>

struct AutonomyConfig {
    bool enabled = false, paused = false;
    int coalesceWindowMs = 1000, coalesceMaxWaitMs = 3000;
    int minDecisionIntervalMs = 60000, noOpBackoffMs = 300000;
    int maxNoOpBackoffMs = 1800000, localTickMs = 15000, ticketTtlMs = 120000;
    int maxSelectionCallsPerHour = 12;
    qint64 maxActiveMsPerHour = 900000, maxTokensPerHour = 120000;
    qint64 maxEstimatedCostMicrosPerHour = 1000000, unknownAttemptCostMicros = 50000;
    static AutonomyConfig fromJson(const QJsonObject &json);
};
struct OpportunityClock {
    QDateTime utc;
    qint64 monotonicMs = 0;
    QString sessionId;
};
struct RuntimeReadiness {
    QString hostEpoch;
    QString foregroundActivity, selectionReservation;
    QStringList readyObligations, pendingResults;
    bool running = false, enabled = true, autonomyEnabled = false;
    bool paused = false, sleeping = false, budgetRemaining = true;
    bool inputPending = false, modelPending = false, toolPending = false;
    QString mode;
    quint64 hostRevision = 0;
};
struct IdleDecision {
    bool isIdle = false, canDeliberate = false;
    QString reason;
    QStringList blockingRefs;
};
IdleDecision evaluateIdle(const RuntimeReadiness &readiness);
struct CoalescedMessage {
    QString contextId, messageId;
    qint64 receivedAtMonotonicMs = 0;
};
struct CoalescedBatch {
    QString contextId;
    QStringList messageIds;
    qint64 dueAtMonotonicMs = 0;
};
CoalescedBatch nextMessageBatch(const QList<CoalescedMessage> &, const AutonomyConfig &);
struct OpportunitySignal {
    QString kind = "tick", contextId;
    QStringList eventIds;
    RuntimeReadiness readiness;
    QJsonObject inputWatermark;
    OpportunityClock clock;
    qint64 expectedRuntimeRevision = -1;
};
struct OpportunityTicket {
    QString ticketId, profileId;
    qint64 runtimeRevision = 0;
    quint64 hostRevision = 0;
    QJsonObject inputWatermark;
    QDateTime expiresAt;
    QString clockSessionId;
    qint64 monotonicDeadline = 0;
    QJsonObject toJson() const;
    static OpportunityTicket fromJson(const QJsonObject &json);
};
struct OpportunityFinish {
    QString ticketId, outcome = "wait", reason;
    quint64 hostRevision = 0;
    QJsonObject inputWatermark;
    QDateTime nextConsiderAt;
    OpportunityClock clock;
};
struct ModelBudgetRequest {
    QString attemptId, profileId, ticketId, activityId, purpose;
    qint64 inputBytes = 0, maxOutputTokens = 0, routeEstimatedCostMicros = 0;
    OpportunityClock clock;
};
struct ModelBudgetReservation {
    QString attemptId;
    qint64 tokens = 0, estimatedCostMicros = 0;
};
struct ModelBudgetSettlement {
    QString profileId, attemptId;
    LlmUsage usage;
    OpportunityClock clock;
};
