#include "emotion_state_provider.h"

#include <cmath>

#include "emotion/emotion_engine.h"

namespace {

QDateTime normalizedTime(const QDateTime& at) {
    return at.isValid() ? at.toUTC() : QDateTime::currentDateTimeUtc();
}
ProvidedEmotionSnapshot neutralSnapshot(const QString& profileId,
                                         const QDateTime& at) {
    ProvidedEmotionSnapshot provided;
    provided.profileId = profileId;
    provided.neutralFallback = true;
    provided.value.moodValence = 0.0;
    provided.value.moodArousal = 0.0;
    provided.value.active = EmotionType::Neutral;
    provided.value.intensity = 0.0;
    provided.value.confidence = 1.0;
    provided.value.sourceEventId.clear();
    provided.value.updatedAt = normalizedTime(at);
    provided.value.expressionExpiresAt = {};
    return provided;
}

bool isValidSnapshot(const EmotionSnapshot& snapshot) {
    return std::isfinite(snapshot.moodValence)
        && snapshot.moodValence >= -1.0
        && snapshot.moodValence <= 1.0
        && std::isfinite(snapshot.moodArousal)
        && snapshot.moodArousal >= 0.0
        && snapshot.moodArousal <= 1.0
        && std::isfinite(snapshot.intensity)
        && snapshot.intensity >= 0.0
        && snapshot.intensity <= 1.0
        && std::isfinite(snapshot.confidence)
        && snapshot.confidence >= 0.0
        && snapshot.confidence <= 1.0;
}

} // namespace

ProvidedEmotionSnapshot NullEmotionStateProvider::currentSnapshot(
    const QString& profileId, const QDateTime& at) const {
    return neutralSnapshot(profileId, at);
}

QList<ProvidedEmotionSnapshot> NullEmotionStateProvider::trajectory(
    const QString& profileId,
    const QDateTime& from,
    const QDateTime& to) const {
    Q_UNUSED(profileId)
    Q_UNUSED(from)
    Q_UNUSED(to)
    return {};
}

EmotionEngineStateProvider::EmotionEngineStateProvider(const EmotionEngine* engine)
    : m_engine(engine) {}

ProvidedEmotionSnapshot EmotionEngineStateProvider::currentSnapshot(
    const QString& profileId, const QDateTime& at) const {
    if (!m_engine || !m_engine->isEnabled()) {
        return neutralSnapshot(profileId, at);
    }

    const QDateTime snapshotTime = normalizedTime(at);
    const EmotionSnapshot snapshot = m_engine->snapshot(snapshotTime);
    if (!isValidSnapshot(snapshot)) {
        return neutralSnapshot(profileId, snapshotTime);
    }

    ProvidedEmotionSnapshot provided;
    provided.profileId = profileId;
    provided.value = snapshot;
    return provided;
}

QList<ProvidedEmotionSnapshot> EmotionEngineStateProvider::trajectory(
    const QString& profileId,
    const QDateTime& from,
    const QDateTime& to) const {
    Q_UNUSED(profileId)
    Q_UNUSED(from)
    Q_UNUSED(to)
    return {};
}
