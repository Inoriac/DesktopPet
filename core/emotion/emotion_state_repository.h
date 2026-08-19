#ifndef DESKTOP_PET_EMOTION_STATE_REPOSITORY_H
#define DESKTOP_PET_EMOTION_STATE_REPOSITORY_H

#include <QDateTime>

#include <optional>

struct PersistedEmotionState {
    int schemaVersion = 1;
    double moodValence = 0.10;
    double moodArousal = 0.35;
    QDateTime updatedAtUtc;
    int personalityRevision = 1;
};

class IEmotionStateRepository {
public:
    virtual ~IEmotionStateRepository() = default;

    virtual std::optional<PersistedEmotionState> load() = 0;
    virtual bool save(const PersistedEmotionState& state) = 0;
};

#endif // DESKTOP_PET_EMOTION_STATE_REPOSITORY_H
