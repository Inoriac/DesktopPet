#ifndef DESKTOP_PET_PROACTIVE_CHAT_POLICY_H
#define DESKTOP_PET_PROACTIVE_CHAT_POLICY_H

#include "ai/identity/identity_baseline.h"
#include "emotion/emotion_types.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

struct ProactiveChatTiming {
    int intervalMs = 0;
    int cooldownMs = 0;
    double willingness = 1.0;

    int remainingMs(qint64 elapsedMs) const {
        return static_cast<int>(std::clamp<qint64>(
            static_cast<qint64>(intervalMs) - std::max<qint64>(0, elapsedMs),
            0, std::numeric_limits<int>::max()));
    }
};

// Continuous modulation of the user's configured pace, not an emotion-to-timer table.
inline ProactiveChatTiming calculateProactiveChatTiming(
    int baseIntervalMs, const QMap<QString, double>& traits,
    const std::optional<EmotionSnapshot>& emotion, int unanswered) {
    const auto trait = [&traits](const QString& name, double fallback) {
        const double value = traits.value(name, fallback);
        return std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : fallback;
    };
    const double drive = 0.55 * trait(QStringLiteral("initiative"), 0.35)
        + 0.35 * trait(QStringLiteral("sociability"), 0.45)
        + 0.10 * trait(QStringLiteral("openness"), 0.60);
    double mood = 0.0;
    if (emotion && std::isfinite(emotion->moodValence)
        && std::isfinite(emotion->moodArousal)
        && std::isfinite(emotion->intensity) && std::isfinite(emotion->confidence)) {
        const double valence = std::clamp(emotion->moodValence, -1.0, 1.0);
        const double arousal = std::clamp(emotion->moodArousal, 0.0, 1.0);
        // Agitated negative emotions must not produce more unsolicited interruptions.
        mood = 0.35 * (valence - 0.10)
            + 0.35 * (arousal - 0.35) * std::max(0.0, valence);
        double expression = 0.0;
        switch (emotion->active) {
        case EmotionType::Joy: expression = 0.25; break;
        case EmotionType::Surprise: expression = 0.15; break;
        case EmotionType::Sadness: expression = -0.30; break;
        case EmotionType::Anger: expression = -0.40; break;
        case EmotionType::Fear: expression = -0.35; break;
        case EmotionType::Neutral: break;
        }
        mood = (mood + expression * std::clamp(emotion->intensity, 0.0, 1.0))
            * std::clamp(emotion->confidence, 0.0, 1.0);
    }
    const double willingness = std::clamp(std::exp(1.6 * (drive - 0.41) + mood),
                                           0.5, 1.8);
    const int count = std::clamp(unanswered, 0, 3);
    const double backoff = 1.0 + count * (0.55 - 0.25 * drive);
    const double interval = std::max(1000, baseIntervalMs) * backoff / willingness;
    const double cooldown = 120000.0 / willingness
        * (1.0 + std::max(0, count - 1) * (0.65 - 0.25 * drive));
    return {
        static_cast<int>(std::clamp(interval, 60000.0,
                                   static_cast<double>(std::numeric_limits<int>::max()))),
        static_cast<int>(std::clamp(cooldown, 60000.0, 600000.0)),
        willingness
    };
}

#endif
