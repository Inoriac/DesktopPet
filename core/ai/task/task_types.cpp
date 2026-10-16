#include "task_types.h"
#include "ai/chat/conversation_types.h"

bool TaskRecord::terminal() const {
    return status == "Completed" || status == "Declined" || status == "Failed" || status == "Cancelled";
}

QJsonObject TaskRecord::toJson() const {
    return {{"pause_reason", hostPauseReason},
            {"activity_intent", activityIntent},
            {"context", contextSpecToJson(context)},
            {"autonomous", autonomous},
            {"activity_revision", activityRevision},
            {"activity_generation", qint64(activityGeneration)},
            {"id", id},
            {"profile_id", profileId},
            {"subject_id", subjectId},
            {"origin_kind", originKindToString(originKind)},
            {"goal", goal},
            {"status", status},
            {"plan", plan},
            {"result", result},
            {"reason", reason},
            {"completion_criteria", completionCriteria},
            {"personality_version", personalityVersion},
            {"required_source_tools",
             QJsonArray::fromStringList(requiredSourceTools)},
            {"personality_snapshot", personalitySnapshot},
            {"relationship_snapshot", relationshipSnapshot},
            {"initial_emotion", initialEmotion},
            {"current_emotion", currentEmotion},
            {"steps", steps},
            {"decisions", decisions},
            {"evidence", evidence},
            {"checkpoints", checkpoints},
            {"handled_triggers", handledTriggers},
            {"pending_trigger", pendingTrigger},
            {"user_feedback", userFeedback},
            {"requirement_updates", requirementUpdates},
            {"created_at", createdAt},
            {"updated_at", updatedAt},
            {"tool_calls", toolCalls},
            {"replans", replans},
            {"consecutive_failures", consecutiveFailures},
            {"active_ms", activeMs},
            {"last_agency_active_ms", lastAgencyActiveMs},
            {"accepted", accepted},
            {"needs_revalidation", needsRevalidation},
            {"budget", QJsonObject{{"max_tool_calls", budget.maxToolCalls},
                                   {"max_replans", budget.maxReplans},
                                   {"max_active_ms", budget.maxActiveMs},
                                   {"model_timeout_ms", budget.modelTimeoutMs},
                                   {"tool_timeout_ms", budget.toolTimeoutMs}}}};
}

TaskRecord TaskRecord::fromJson(const QJsonObject& j) {
    TaskRecord r;
    r.activityIntent = j["activity_intent"].toObject();
    r.hostPauseReason = j["pause_reason"].toString();
    r.autonomous = j["autonomous"].toBool(false);
    r.context = contextSpecFromJson(j["context"].toObject());
    r.activityRevision = j["activity_revision"].toInteger();
    r.activityGeneration = quint64(j["activity_generation"].toInteger());
    r.id = j.value("id").toString(); r.profileId = j.value("profile_id").toString();
    r.subjectId = j.value("subject_id").toString(); r.goal = j.value("goal").toString();
    r.originKind = j.contains("origin_kind")
        ? originKindFromString(j.value("origin_kind").toString()).value_or(OriginKind::Internal)
        : OriginKind::LocalOwner;
    r.status = j.value("status").toString("Received"); r.plan = j.value("plan").toString();
    r.result = j.value("result").toString(); r.reason = j.value("reason").toString();
    r.completionCriteria = j.value("completion_criteria").toString();
    for (const auto& name : j.value("required_source_tools").toArray()) r.requiredSourceTools.append(name.toString());
    r.personalityVersion = j.value("personality_version").toInteger();
    r.personalitySnapshot = j.value("personality_snapshot").toObject();
    r.relationshipSnapshot = j.value("relationship_snapshot").toObject();
    r.initialEmotion = j.value("initial_emotion").toObject(); r.currentEmotion = j.value("current_emotion").toObject();
    r.steps = j.value("steps").toArray(); r.decisions = j.value("decisions").toArray();
    r.evidence = j.value("evidence").toArray(); r.checkpoints = j.value("checkpoints").toArray();
    r.handledTriggers = j.value("handled_triggers").toArray(); r.pendingTrigger = j.value("pending_trigger").toString();
    r.userFeedback = j.value("user_feedback").toString(); r.createdAt = j.value("created_at").toString();
    r.requirementUpdates = j.value("requirement_updates").toString();
    r.updatedAt = j.value("updated_at").toString(); r.toolCalls = j.value("tool_calls").toInt();
    r.replans = j.value("replans").toInt(); r.consecutiveFailures = j.value("consecutive_failures").toInt();
    r.activeMs = j.value("active_ms").toInteger(); r.lastAgencyActiveMs = j.value("last_agency_active_ms").toInteger(-60000);
    r.accepted = j.value("accepted").toBool(); r.needsRevalidation = j.value("needs_revalidation").toBool();
    const auto b = j.value("budget").toObject();
    r.budget.maxToolCalls = b.value("max_tool_calls").toInt(12);
    r.budget.maxReplans = b.value("max_replans").toInt(2);
    r.budget.maxActiveMs = b.value("max_active_ms").toInteger(300000);
    r.budget.modelTimeoutMs = b.value("model_timeout_ms").toInt(60000);
    r.budget.toolTimeoutMs = b.value("tool_timeout_ms").toInt(30000);
    return r;
}
