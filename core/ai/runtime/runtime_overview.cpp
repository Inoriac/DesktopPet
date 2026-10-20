#include "runtime_overview.h"

QJsonObject runtimeOverviewJson(const RuntimeOverviewInput& input) {
    QJsonObject autonomy;
    for (const auto* key : {"enabled", "ready", "state", "error", "isIdle",
                            "canDeliberate", "reason", "paused"}) {
        if (input.autonomy.contains(key)) autonomy.insert(key, input.autonomy.value(key));
    }
    const bool emotionAvailable = input.emotionEnabled && input.emotion.has_value();
    QJsonObject emotion{{"enabled", input.emotionEnabled}, {"available", emotionAvailable}};
    if (emotionAvailable) {
        const auto& snapshot = *input.emotion;
        emotion.insert("active", emotionTypeToString(snapshot.active));
        emotion.insert("moodValence", snapshot.moodValence);
        emotion.insert("moodArousal", snapshot.moodArousal);
        emotion.insert("intensity", snapshot.intensity);
        emotion.insert("confidence", snapshot.confidence);
        emotion.insert("updatedAt", snapshot.updatedAt.toUTC().toString(Qt::ISODateWithMs));
    }
    return {{"schemaVersion", 1}, {"profileId", input.profileId}, {"petName", input.petName},
        {"capturedAt", input.capturedAt.toUTC().toString(Qt::ISODateWithMs)},
        {"runtime", QJsonObject{{"aiEnabled", input.aiEnabled}, {"storageReady", input.storageReady},
            {"busy", input.busy}, {"pendingMessageCount", input.pendingMessageCount},
            {"autonomy", autonomy}, {"taskStatus", input.taskStatus},
            {"sleeping", input.sleeping}, {"daydreamRunning", input.daydreamRunning}}},
        {"emotion", emotion}, {"statistics", input.statistics}};
}
