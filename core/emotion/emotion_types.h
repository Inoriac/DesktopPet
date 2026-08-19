#ifndef DESKTOP_PET_EMOTION_TYPES_H
#define DESKTOP_PET_EMOTION_TYPES_H

#include <QDateTime>
#include <QMetaType>
#include <QString>

enum class EmotionType {
    Neutral,
    Joy,
    Sadness,
    Anger,
    Fear,
    Surprise
};

enum class AffectEventKind {
    Unspecified,
    Touch,
    TaskSucceeded,
    TaskFailed,
    ToolSucceeded,
    ToolFailed,
    ReminderSucceeded,
    ReminderFailed,
    ExplicitPositiveFeedback,
    ExplicitNegativeFeedback,
    ConfirmedLoss,
    ExternalObstruction,
    UncertainThreat,
    NovelEvent,
    MemoryRecalled,
    UserIdle,
    ApplicationExit,
    UserRefusal,
    UserCorrection,
    PrivacyChanged,
    MemoryDeleted,
    EmotionDisabled
};

enum class AffectSource {
    Unknown,
    User,
    System,
    Tool,
    Reminder,
    Memory
};

enum class AffectAgency {
    Unknown,
    Self,
    User,
    System,
    Environment
};

enum class AffectOutcome {
    Unknown,
    Ongoing,
    Success,
    Failure,
    Loss
};

struct AffectEvent {
    QString id;
    AffectEventKind kind = AffectEventKind::Unspecified;
    AffectSource source = AffectSource::Unknown;
    QString sourceId;
    double goalCongruence = 0.0;
    double novelty = 0.0;
    double certainty = 1.0;
    double controllability = 0.5;
    double relevance = 0.0;
    AffectAgency agency = AffectAgency::Unknown;
    AffectOutcome outcome = AffectOutcome::Unknown;
    double confidence = 1.0;
    QDateTime occurredAt;
};

struct EmotionSnapshot {
    double moodValence = 0.10;
    double moodArousal = 0.35;
    EmotionType active = EmotionType::Neutral;
    double intensity = 0.0;
    double confidence = 1.0;
    QString sourceEventId;
    QDateTime updatedAt;
    QDateTime expressionExpiresAt;

    bool operator==(const EmotionSnapshot&) const = default;
};

struct ExpressionRequest {
    EmotionType emotion = EmotionType::Neutral;
    double intensity = 0.0;
    QDateTime requestedAt;
    QDateTime expiresAt;
    bool allowUnsolicited = false;

    bool operator==(const ExpressionRequest&) const = default;
};

struct EmotionConfig {
    bool enabled = true;

    double baselineValence = 0.10;
    double baselineArousal = 0.35;

    double valenceHalfLifeSec = 3600.0;
    double arousalHalfLifeSec = 1200.0;
    double maxOfflineDecaySec = 21600.0;

    double maxValenceImpulse = 0.18;
    double maxArousalImpulse = 0.25;
    double negativeMultiplier = 0.75;
    int sameSourcePerMinute = 3;

    double positiveThreshold = 0.45;
    double negativeThreshold = 0.65;
    double switchMargin = 0.12;
    double minExpressionIntensity = 0.45;
    qint64 expressionDurationMs = 12000;
    qint64 expressionCooldownMs = 60000;
    int expressionQueueLimit = 3;

    bool llmAppraisalEnabled = false;
    double llmMinConfidence = 0.80;
    int personalityRevision = 1;
};

EmotionConfig sanitizeEmotionConfig(const EmotionConfig& config);

inline QString emotionTypeToString(EmotionType emotion) {
    switch (emotion) {
    case EmotionType::Neutral:
        return QStringLiteral("neutral");
    case EmotionType::Joy:
        return QStringLiteral("joy");
    case EmotionType::Sadness:
        return QStringLiteral("sadness");
    case EmotionType::Anger:
        return QStringLiteral("anger");
    case EmotionType::Fear:
        return QStringLiteral("fear");
    case EmotionType::Surprise:
        return QStringLiteral("surprise");
    }
    return QStringLiteral("neutral");
}

inline EmotionType emotionTypeFromString(const QString& value) {
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("joy")) return EmotionType::Joy;
    if (normalized == QStringLiteral("sadness")) return EmotionType::Sadness;
    if (normalized == QStringLiteral("anger")) return EmotionType::Anger;
    if (normalized == QStringLiteral("fear")) return EmotionType::Fear;
    if (normalized == QStringLiteral("surprise")) return EmotionType::Surprise;
    return EmotionType::Neutral;
}

QString affectSourceToString(AffectSource source);
bool isNegativeEmotion(EmotionType emotion);
bool isProtectedAffectEvent(AffectEventKind kind);

Q_DECLARE_METATYPE(EmotionType)
Q_DECLARE_METATYPE(EmotionSnapshot)
Q_DECLARE_METATYPE(ExpressionRequest)

#endif // DESKTOP_PET_EMOTION_TYPES_H
