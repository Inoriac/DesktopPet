#include "activity_types.h"
QString activityStateName(ActivityState state) {
    static const QStringList names{"Ready",       "Running", "WaitingExternal",
                                   "WaitingUser", "Paused",  "Completed",
                                   "Cancelled",   "Failed"};
    return names.at(int(state));
}
std::optional<ActivityState> activityStateFromName(const QString &value) {
    for (int i = 0; i <= int(ActivityState::Failed); ++i)
        if (activityStateName(ActivityState(i)) == value)
            return ActivityState(i);
    return {};
}
bool activityTerminal(ActivityState state) {
    return state == ActivityState::Completed ||
           state == ActivityState::Cancelled || state == ActivityState::Failed;
}
QJsonObject ActivityBudget::toJson() const {
    return {{"limits", QJsonObject{{"max_tool_calls", limits.maxToolCalls},
                                   {"max_replans", limits.maxReplans},
                                   {"max_active_ms", limits.maxActiveMs},
                                   {"model_timeout_ms", limits.modelTimeoutMs},
                                   {"tool_timeout_ms", limits.toolTimeoutMs}}},
            {"used", QJsonObject{{"tool_calls", toolCalls},
                                 {"replans", replans},
                                 {"active_ms", activeMs}}},
            {"reservations", QJsonObject{}}};
}
ActivityBudget ActivityBudget::fromJson(const QJsonObject &j) {
    ActivityBudget b;
    const auto limits = j["limits"].toObject(), used = j["used"].toObject();
    b.limits.maxToolCalls = limits["max_tool_calls"].toInt(12);
    b.limits.maxReplans = limits["max_replans"].toInt(2);
    b.limits.maxActiveMs = limits["max_active_ms"].toInteger(300000);
    b.limits.modelTimeoutMs = limits["model_timeout_ms"].toInt(60000);
    b.limits.toolTimeoutMs = limits["tool_timeout_ms"].toInt(30000);
    b.toolCalls = used["tool_calls"].toInt();
    b.replans = used["replans"].toInt();
    b.activeMs = used["active_ms"].toInteger();
    return b;
}
QJsonObject ActivityIntent::toJson() const {
    QJsonArray refs;
    for (const auto &ref : capabilityRefs)
        refs.append(ref.toJson());
    return {{"goal", goal},
            {"reason", reason},
            {"origin", origin},
            {"stopWhen", stopWhen},
            {"reconsiderWhen", reconsiderWhen},
            {"evidenceRefs", QJsonArray::fromStringList(evidenceRefs)},
            {"capabilityRefs", refs}};
}
