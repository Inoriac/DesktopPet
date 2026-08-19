#include "emotion_config_json.h"

#include <cmath>
#include <limits>

namespace {

double numberValue(const QJsonObject& object, const QString& key, double fallback) {
    const QJsonValue value = object.value(key);
    if (!value.isDouble()) {
        return fallback;
    }
    const double number = value.toDouble(fallback);
    return std::isfinite(number) ? number : fallback;
}

qint64 integerValue(const QJsonObject& object, const QString& key, qint64 fallback) {
    const QJsonValue value = object.value(key);
    if (!value.isDouble()) {
        return fallback;
    }
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::trunc(number) != number) {
        return fallback;
    }
    const double largestSafeQint64 = std::nextafter(
        static_cast<double>(std::numeric_limits<qint64>::max()), 0.0);
    if (number < static_cast<double>(std::numeric_limits<qint64>::lowest())
        || number > largestSafeQint64) {
        return fallback;
    }
    return static_cast<qint64>(number);
}

int intValue(const QJsonObject& object, const QString& key, int fallback) {
    const qint64 value = integerValue(object, key, fallback);
    if (value < std::numeric_limits<int>::lowest()
        || value > std::numeric_limits<int>::max()) {
        return fallback;
    }
    return static_cast<int>(value);
}

bool boolValue(const QJsonObject& object, const QString& key, bool fallback) {
    const QJsonValue value = object.value(key);
    return value.isBool() ? value.toBool() : fallback;
}

QJsonObject childObject(const QJsonObject& object, const QString& key) {
    const QJsonValue value = object.value(key);
    return value.isObject() ? value.toObject() : QJsonObject{};
}

} // namespace

EmotionConfig parseEmotionConfig(const QJsonObject& object) {
    EmotionConfig config;
    config.enabled = boolValue(object, QStringLiteral("enabled"), config.enabled);

    const QJsonObject baseline = childObject(object, QStringLiteral("baseline"));
    config.baselineValence = numberValue(
        baseline, QStringLiteral("valence"), config.baselineValence);
    config.baselineArousal = numberValue(
        baseline, QStringLiteral("arousal"), config.baselineArousal);

    const QJsonObject decay = childObject(object, QStringLiteral("decay"));
    config.valenceHalfLifeSec = numberValue(
        decay, QStringLiteral("valenceHalfLifeSec"), config.valenceHalfLifeSec);
    config.arousalHalfLifeSec = numberValue(
        decay, QStringLiteral("arousalHalfLifeSec"), config.arousalHalfLifeSec);
    config.maxOfflineDecaySec = numberValue(
        decay, QStringLiteral("maxOfflineDecaySec"), config.maxOfflineDecaySec);

    const QJsonObject impulse = childObject(object, QStringLiteral("impulse"));
    config.maxValenceImpulse = numberValue(
        impulse, QStringLiteral("maxValence"), config.maxValenceImpulse);
    config.maxArousalImpulse = numberValue(
        impulse, QStringLiteral("maxArousal"), config.maxArousalImpulse);
    config.negativeMultiplier = numberValue(
        impulse, QStringLiteral("negativeMultiplier"), config.negativeMultiplier);
    config.sameSourcePerMinute = intValue(
        impulse, QStringLiteral("sameSourcePerMinute"), config.sameSourcePerMinute);

    const QJsonObject expression = childObject(object, QStringLiteral("expression"));
    config.positiveThreshold = numberValue(
        expression, QStringLiteral("positiveThreshold"), config.positiveThreshold);
    config.negativeThreshold = numberValue(
        expression, QStringLiteral("negativeThreshold"), config.negativeThreshold);
    config.switchMargin = numberValue(
        expression, QStringLiteral("switchMargin"), config.switchMargin);
    config.minExpressionIntensity = numberValue(
        expression, QStringLiteral("minIntensity"), config.minExpressionIntensity);
    config.expressionDurationMs = integerValue(
        expression, QStringLiteral("durationMs"), config.expressionDurationMs);
    config.expressionCooldownMs = integerValue(
        expression, QStringLiteral("cooldownMs"), config.expressionCooldownMs);
    config.expressionQueueLimit = intValue(
        expression, QStringLiteral("queueLimit"), config.expressionQueueLimit);

    const QJsonObject llmAppraisal = childObject(object, QStringLiteral("llmAppraisal"));
    config.llmAppraisalEnabled = boolValue(
        llmAppraisal, QStringLiteral("enabled"), config.llmAppraisalEnabled);
    config.llmMinConfidence = numberValue(
        llmAppraisal, QStringLiteral("minConfidence"), config.llmMinConfidence);
    config.personalityRevision = intValue(
        object, QStringLiteral("personalityRevision"), config.personalityRevision);
    return sanitizeEmotionConfig(config);
}
