#ifndef DESKTOP_PET_RUNTIME_OVERVIEW_H
#define DESKTOP_PET_RUNTIME_OVERVIEW_H

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <optional>

#include "emotion/emotion_types.h"

// Read-only values collected by the host; the overview owns no runtime services.
struct RuntimeOverviewInput {
    QString profileId;
    QString petName;
    QDateTime capturedAt;
    bool aiEnabled = false;
    bool storageReady = false;
    bool busy = false;
    int pendingMessageCount = 0;
    QJsonObject autonomy;
    QString taskStatus;
    bool sleeping = false;
    bool daydreamRunning = false;
    bool emotionEnabled = false;
    std::optional<EmotionSnapshot> emotion;
    QJsonObject statistics;
};

QJsonObject runtimeOverviewJson(const RuntimeOverviewInput& input);

#endif // DESKTOP_PET_RUNTIME_OVERVIEW_H
