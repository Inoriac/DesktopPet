#include "opportunity_types.h"
#include <algorithm>

AutonomyConfig AutonomyConfig::fromJson(const QJsonObject &j) {
    AutonomyConfig c;
    c.enabled = j["enabled"].toBool(false);
    c.paused = j["paused"].toBool(false);
    const auto access = j["memoryAccess"].toObject();
    bool valid = !j.contains("memoryAccess") ||
                 (j["memoryAccess"].isObject() &&
                  (!access.contains("owner") || access["owner"].isBool()) &&
                  (!access.contains("conversationIds") ||
                   access["conversationIds"].isArray()));
    c.memoryAccessOwner = access["owner"].toBool(false);
    for (const auto &id : access["conversationIds"].toArray()) {
        if (!id.isString() || id.toString().trimmed().isEmpty() ||
            id.toString().size() > 128)
            valid = false;
        else if (!c.memoryAccessConversationIds.contains(id.toString()))
            c.memoryAccessConversationIds << id.toString();
    }
    if (c.memoryAccessConversationIds.size() > 32)
        valid = false;
    if (!valid) {
        c.memoryAccessOwner = false;
        c.memoryAccessConversationIds.clear();
    }
    auto number = [&j](const char *key, qint64 fallback, qint64 maximum) {
        const auto v = j[key];
        return v.isDouble() && v.toDouble() == double(v.toInteger()) && v.toInteger() > 0 &&
                       v.toInteger() <= maximum
                   ? v.toInteger()
                   : fallback;
    };
#define SET_CONFIG(name, maximum) c.name = number(#name, c.name, maximum)
    SET_CONFIG(coalesceWindowMs, 3000);
    SET_CONFIG(coalesceMaxWaitMs, 10000);
    SET_CONFIG(minDecisionIntervalMs, 3600000);
    SET_CONFIG(noOpBackoffMs, 3600000);
    SET_CONFIG(maxNoOpBackoffMs, 86400000);
    SET_CONFIG(localTickMs, 60000);
    SET_CONFIG(ticketTtlMs, 600000);
    SET_CONFIG(maxSelectionCallsPerHour, 1000);
    SET_CONFIG(maxActiveMsPerHour, 3600000);
    SET_CONFIG(maxTokensPerHour, 1000000000);
    SET_CONFIG(maxEstimatedCostMicrosPerHour, 1000000000000LL);
    SET_CONFIG(unknownAttemptCostMicros, 1000000000000LL);
#undef SET_CONFIG
    c.coalesceMaxWaitMs = std::max(c.coalesceWindowMs, c.coalesceMaxWaitMs);
    c.maxNoOpBackoffMs = std::max(c.noOpBackoffMs, c.maxNoOpBackoffMs);
    return c;
}
IdleDecision evaluateIdle(const RuntimeReadiness &r) {
    IdleDecision d;
    if (!r.foregroundActivity.isEmpty())
        d.blockingRefs << r.foregroundActivity;
    if (!r.selectionReservation.isEmpty())
        d.blockingRefs << r.selectionReservation;
    d.blockingRefs << r.readyObligations << r.pendingResults;
    if (r.inputPending)
        d.blockingRefs << "input";
    if (r.modelPending)
        d.blockingRefs << "model";
    if (r.toolPending)
        d.blockingRefs << "tool";
    d.isIdle = d.blockingRefs.isEmpty();
    if (!d.isIdle)
        d.reason = "busy";
    else if (!r.running || !r.enabled)
        d.reason = "stopped";
    else if (!r.autonomyEnabled)
        d.reason = "disabled";
    else if (r.paused)
        d.reason = "paused";
    else if (r.sleeping)
        d.reason = "sleeping";
    else if (r.mode == "quiet" || r.mode == "focus")
        d.reason = r.mode;
    else if (!r.budgetRemaining)
        d.reason = "budget";
    else {
        d.reason = "idle";
        d.canDeliberate = true;
    }
    return d;
}
QJsonObject OpportunityTicket::toJson() const {
    return {{"hostEpoch", hostEpoch},
            {"contextId", contextId},
            {"eventIds", QJsonArray::fromStringList(eventIds)},
            {"ticketId", ticketId},
            {"profileId", profileId},
            {"runtimeRevision", runtimeRevision},
            {"hostRevision", qint64(hostRevision)},
            {"inputWatermark", inputWatermark},
            {"expiresAt", expiresAt.toUTC().toString(Qt::ISODateWithMs)},
            {"clockSessionId", clockSessionId},
            {"monotonicDeadline", monotonicDeadline}};
}
OpportunityTicket OpportunityTicket::fromJson(const QJsonObject &j) {
    OpportunityTicket t;
    t.hostEpoch = j["hostEpoch"].toString();
    t.contextId = j["contextId"].toString();
    for (const auto &id : j["eventIds"].toArray())
        t.eventIds << id.toString();
    t.ticketId = j["ticketId"].toString();
    t.profileId = j["profileId"].toString();
    t.runtimeRevision = j["runtimeRevision"].toInteger();
    t.hostRevision = j["hostRevision"].toInteger();
    t.inputWatermark = j["inputWatermark"].toObject();
    t.expiresAt = QDateTime::fromString(j["expiresAt"].toString(), Qt::ISODateWithMs);
    t.clockSessionId = j["clockSessionId"].toString();
    t.monotonicDeadline = j["monotonicDeadline"].toInteger();
    return t;
}

CoalescedBatch nextMessageBatch(const QList<CoalescedMessage> &messages,
                                const AutonomyConfig &config) {
    QMap<QString, CoalescedBatch> batches;
    QMap<QString, qint64> first;
    QStringList order;
    for (const auto &message : messages) {
        if (!first.contains(message.contextId)) {
            first[message.contextId] = message.receivedAtMonotonicMs;
            order << message.contextId;
        }
        const auto deadline = first[message.contextId] + config.coalesceMaxWaitMs;
        if (message.receivedAtMonotonicMs > deadline)
            continue;
        auto &batch = batches[message.contextId];
        batch.contextId = message.contextId;
        batch.messageIds << message.messageId;
        batch.dueAtMonotonicMs =
            qMin(deadline, message.receivedAtMonotonicMs + config.coalesceWindowMs);
    }
    CoalescedBatch result;
    for (const auto &id : order)
        if (result.messageIds.isEmpty() || batches[id].dueAtMonotonicMs < result.dueAtMonotonicMs)
            result = batches[id];
    return result;
}
