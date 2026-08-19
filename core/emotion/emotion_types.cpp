#include "emotion_types.h"

#include <algorithm>
#include <cmath>

namespace {

double finiteOr(double value, double fallback) {
    return std::isfinite(value) ? value : fallback;
}

} // namespace

EmotionConfig sanitizeEmotionConfig(const EmotionConfig& input) {
    const EmotionConfig defaults;
    EmotionConfig config = input;

    config.baselineValence = std::clamp(
        finiteOr(config.baselineValence, defaults.baselineValence), -1.0, 1.0);
    config.baselineArousal = std::clamp(
        finiteOr(config.baselineArousal, defaults.baselineArousal), 0.0, 1.0);

    config.valenceHalfLifeSec = std::clamp(
        finiteOr(config.valenceHalfLifeSec, defaults.valenceHalfLifeSec), 1.0, 30.0 * 24.0 * 3600.0);
    config.arousalHalfLifeSec = std::clamp(
        finiteOr(config.arousalHalfLifeSec, defaults.arousalHalfLifeSec), 1.0, 30.0 * 24.0 * 3600.0);
    config.maxOfflineDecaySec = std::clamp(
        finiteOr(config.maxOfflineDecaySec, defaults.maxOfflineDecaySec), 0.0, 30.0 * 24.0 * 3600.0);

    config.maxValenceImpulse = std::clamp(
        finiteOr(config.maxValenceImpulse, defaults.maxValenceImpulse), 0.0, 1.0);
    config.maxArousalImpulse = std::clamp(
        finiteOr(config.maxArousalImpulse, defaults.maxArousalImpulse), 0.0, 1.0);
    config.negativeMultiplier = std::clamp(
        finiteOr(config.negativeMultiplier, defaults.negativeMultiplier), 0.0, 1.0);
    config.sameSourcePerMinute = std::clamp(config.sameSourcePerMinute, 1, 100);

    config.positiveThreshold = std::clamp(
        finiteOr(config.positiveThreshold, defaults.positiveThreshold), 0.0, 1.0);
    config.negativeThreshold = std::clamp(
        finiteOr(config.negativeThreshold, defaults.negativeThreshold), 0.0, 1.0);
    config.negativeThreshold = std::max(config.negativeThreshold, config.positiveThreshold);
    config.switchMargin = std::clamp(
        finiteOr(config.switchMargin, defaults.switchMargin), 0.0, 1.0);
    config.minExpressionIntensity = std::clamp(
        finiteOr(config.minExpressionIntensity, defaults.minExpressionIntensity), 0.0, 1.0);
    config.expressionDurationMs = std::clamp<qint64>(config.expressionDurationMs, 100, 10 * 60 * 1000);
    config.expressionCooldownMs = std::clamp<qint64>(config.expressionCooldownMs, 0, 24 * 60 * 60 * 1000);
    config.expressionQueueLimit = std::clamp(config.expressionQueueLimit, 0, 32);

    config.llmMinConfidence = std::clamp(
        finiteOr(config.llmMinConfidence, defaults.llmMinConfidence), 0.0, 1.0);
    config.personalityRevision = std::max(config.personalityRevision, 1);
    return config;
}

QString affectSourceToString(AffectSource source) {
    switch (source) {
    case AffectSource::User:
        return QStringLiteral("user");
    case AffectSource::System:
        return QStringLiteral("system");
    case AffectSource::Tool:
        return QStringLiteral("tool");
    case AffectSource::Reminder:
        return QStringLiteral("reminder");
    case AffectSource::Memory:
        return QStringLiteral("memory");
    case AffectSource::Unknown:
        return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

bool isNegativeEmotion(EmotionType emotion) {
    return emotion == EmotionType::Sadness
        || emotion == EmotionType::Anger
        || emotion == EmotionType::Fear;
}

bool isProtectedAffectEvent(AffectEventKind kind) {
    switch (kind) {
    case AffectEventKind::MemoryRecalled:
    case AffectEventKind::UserIdle:
    case AffectEventKind::ApplicationExit:
    case AffectEventKind::UserRefusal:
    case AffectEventKind::UserCorrection:
    case AffectEventKind::PrivacyChanged:
    case AffectEventKind::MemoryDeleted:
    case AffectEventKind::EmotionDisabled:
        return true;
    default:
        return false;
    }
}
