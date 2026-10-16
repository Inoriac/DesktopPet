#pragma once
#include "activity_types.h"
#include "ai/scheduler/opportunity_types.h"
#include <QSet>

struct SelectionCue {
    QString id, kind, text;
    QStringList sourceRefs;
};
struct SelectionRequest {
    OpportunityTicket ticket;
    ContextSpec context, internalMemoryScope;
    quint64 catalogRevision = 0;
    QJsonObject identity;
    QList<SelectionCue> cues;
    QList<ActivitySnapshot> activities;
    QJsonArray capabilities;
    QSet<QString> references;
    QStringList unavailable;
    QString memoryDatabasePath, runtimeDatabasePath;
    std::function<Result<void, DomainError>()> validateSources;
};
struct SelectionProposal {
    QString decision, reason, activityId;
    ActivityIntent intent;
    QStringList evidenceRefs;
    qint64 expectedRevision = 0;
    QDateTime nextConsiderAt;
};
struct SelectionAdoption {
    SelectionRequest
        request; // immutable host material authority, never model output
    SelectionProposal proposal;
};
inline bool selectionResumeEligible(const ActivitySnapshot &a) {
    return (a.intent["origin"] == "self" || a.intent["origin"] == "event") &&
           (a.state == ActivityState::Ready ||
            (a.state == ActivityState::Paused &&
             (a.pauseReason == "checkpoint" || a.pauseReason == "budget" ||
              a.pauseReason == "dependency"))) &&
           a.budget.activeMs < a.budget.limits.maxActiveMs &&
           a.budget.toolCalls < a.budget.limits.maxToolCalls;
}

inline bool selectionScopeEquivalent(ContextSpec a, ContextSpec b) {
    a.activityId.clear();
    b.activityId.clear();
    return a.cacheKey() == b.cacheKey();
}
inline bool selectionMemoryScopeContains(const ContextSpec &host,
                                         const ContextSpec &activity) {
    if (activity.originKind != OriginKind::Internal)
        return true;
    if (host.profileId != activity.profileId ||
        (activity.allowOwnerMemory && !host.allowOwnerMemory) ||
        (activity.allowInternalMemory && !host.allowInternalMemory))
        return false;
    for (const auto &id : activity.authorizedConversationIds)
        if (!host.authorizedConversationIds.contains(id))
            return false;
    return true;
}
