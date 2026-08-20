#include "emotion_engine.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr int kSeenEventLimit = 4096;
constexpr qint64 kSourceWindowMs = 60 * 1000;
constexpr qint64 kMaxFutureEventSkewMs = 5 * 60 * 1000;

bool inUnitRange(double value) {
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

bool inSignedUnitRange(double value) {
    return std::isfinite(value) && value >= -1.0 && value <= 1.0;
}

double decayToward(double value, double baseline, double elapsedSec, double halfLifeSec) {
    const double factor = std::exp2(-elapsedSec / halfLifeSec);
    return baseline + (value - baseline) * factor;
}

int emotionIndex(EmotionType emotion) {
    return static_cast<int>(emotion);
}

bool snapshotsDiffer(const EmotionSnapshot& left, const EmotionSnapshot& right) {
    return !(left == right);
}

} // namespace

EmotionEngine::EmotionEngine(EmotionConfig config,
                             IEmotionStateRepository* repository,
                             QObject* parent)
    : QObject(parent),
      m_config(sanitizeEmotionConfig(config)),
      m_repository(repository),
      m_enabled(m_config.enabled),
      m_state(baselineSnapshot({})) {
    qRegisterMetaType<EmotionType>();
    qRegisterMetaType<EmotionSnapshot>();
    qRegisterMetaType<ExpressionRequest>();
    m_lastExpressionAtMs.fill(std::numeric_limits<qint64>::min());
}

bool EmotionEngine::submitEvent(const AffectEvent& event, const QDateTime& nowUtc) {
    if (!m_enabled || !validateEvent(event, nowUtc) || isProtectedAffectEvent(event.kind)) {
        return false;
    }

    const QString eventId = event.id.trimmed();
    if (m_seenEventIds.contains(eventId)) {
        return false;
    }
    rememberEventId(eventId);

    const QDateTime now = nowUtc.toUTC();
    if (!allowSource(event, now.toMSecsSinceEpoch())) {
        return false;
    }

    const EmotionSnapshot before = m_state;
    advanceStateTo(now);

    const Evaluation evaluation = evaluate(event);
    m_state.moodValence = std::clamp(
        m_state.moodValence + evaluation.valenceImpulse, -1.0, 1.0);
    m_state.moodArousal = std::clamp(
        m_state.moodArousal + evaluation.arousalImpulse, 0.0, 1.0);
    m_state.updatedAt = now;

    ExpressionRequest request;
    const bool hasRequest = applyEvaluation(evaluation, event, now, &request);
    const bool changed = snapshotsDiffer(before, m_state);
    if (changed) {
        persist();
        emit stateChanged(m_state);
    }
    if (hasRequest) {
        emit expressionRequested(request);
    }
    return true;
}

void EmotionEngine::advanceTo(const QDateTime& nowUtc) {
    if (!m_enabled || !nowUtc.isValid()) {
        return;
    }
    const EmotionSnapshot before = m_state;
    if (advanceStateTo(nowUtc.toUTC()) && snapshotsDiffer(before, m_state)) {
        emit stateChanged(m_state);
    }
}

EmotionSnapshot EmotionEngine::snapshot(const QDateTime& nowUtc) const {
    EmotionSnapshot projected = m_state;
    if (!m_enabled || !nowUtc.isValid() || !projected.updatedAt.isValid()) {
        return projected;
    }

    const QDateTime now = nowUtc.toUTC();
    if (now <= projected.updatedAt) {
        return projected;
    }
    const double elapsedSec = projected.updatedAt.msecsTo(now) / 1000.0;
    projected.moodValence = std::clamp(
        decayToward(projected.moodValence,
                    m_config.baselineValence,
                    elapsedSec,
                    m_config.valenceHalfLifeSec),
        -1.0,
        1.0);
    projected.moodArousal = std::clamp(
        decayToward(projected.moodArousal,
                    m_config.baselineArousal,
                    elapsedSec,
                    m_config.arousalHalfLifeSec),
        0.0,
        1.0);
    if (projected.expressionExpiresAt.isValid() && now >= projected.expressionExpiresAt) {
        projected.active = EmotionType::Neutral;
        projected.intensity = 0.0;
        projected.confidence = 1.0;
        projected.sourceEventId.clear();
        projected.expressionExpiresAt = {};
    }
    projected.updatedAt = now;
    return projected;
}

bool EmotionEngine::restore(const QDateTime& nowUtc) {
    if (!nowUtc.isValid()) {
        return false;
    }
    const QDateTime now = nowUtc.toUTC();
    if (!m_enabled) {
        const EmotionSnapshot before = m_state;
        m_state = baselineSnapshot(now);
        if (snapshotsDiffer(before, m_state)) {
            emit stateChanged(m_state);
        }
        return false;
    }
    if (!m_repository) {
        reset(now);
        return false;
    }

    const std::optional<PersistedEmotionState> stored = m_repository->load();
    if (!stored.has_value()
        || stored->schemaVersion != 1
        || !inSignedUnitRange(stored->moodValence)
        || !inUnitRange(stored->moodArousal)
        || !stored->updatedAtUtc.isValid()
        || now.msecsTo(stored->updatedAtUtc.toUTC()) > kMaxFutureEventSkewMs
        || stored->personalityRevision != m_config.personalityRevision) {
        reset(now);
        return false;
    }

    const EmotionSnapshot before = m_state;
    const QDateTime updatedAt = stored->updatedAtUtc.toUTC();
    const double offlineSec = std::max(0.0, updatedAt.msecsTo(now) / 1000.0);
    m_state = baselineSnapshot(now);
    if (offlineSec <= m_config.maxOfflineDecaySec) {
        m_state.moodValence = std::clamp(
            decayToward(stored->moodValence,
                        m_config.baselineValence,
                        offlineSec,
                        m_config.valenceHalfLifeSec),
            -1.0,
            1.0);
        m_state.moodArousal = std::clamp(
            decayToward(stored->moodArousal,
                        m_config.baselineArousal,
                        offlineSec,
                        m_config.arousalHalfLifeSec),
            0.0,
            1.0);
    }
    if (snapshotsDiffer(before, m_state)) {
        emit stateChanged(m_state);
    }
    return true;
}

void EmotionEngine::reset(const QDateTime& nowUtc) {
    const QDateTime now = nowUtc.isValid() ? nowUtc.toUTC() : QDateTime::currentDateTimeUtc();
    const EmotionSnapshot before = m_state;
    m_state = baselineSnapshot(now);
    m_seenEventIds.clear();
    m_seenEventOrder.clear();
    m_sourceEvents.clear();
    m_lastExpressionAtMs.fill(std::numeric_limits<qint64>::min());
    persist();
    if (snapshotsDiffer(before, m_state)) {
        emit stateChanged(m_state);
    }
}

void EmotionEngine::setEnabled(bool enabled, const QDateTime& nowUtc) {
    if (m_enabled == enabled) {
        return;
    }
    m_enabled = enabled;
    reset(nowUtc);
}

bool EmotionEngine::validateEvent(const AffectEvent& event, const QDateTime& nowUtc) const {
    if (!nowUtc.isValid()) {
        return false;
    }
    const QDateTime now = nowUtc.toUTC();
    if (m_state.updatedAt.isValid() && now < m_state.updatedAt) {
        return false;
    }
    const QString id = event.id.trimmed();
    const int kind = static_cast<int>(event.kind);
    const int source = static_cast<int>(event.source);
    const int agency = static_cast<int>(event.agency);
    const int outcome = static_cast<int>(event.outcome);
    if (id.isEmpty()
        || id.size() > 128
        || event.sourceId.size() > 128
        || kind <= static_cast<int>(AffectEventKind::Unspecified)
        || kind > static_cast<int>(AffectEventKind::EmotionDisabled)
        || source <= static_cast<int>(AffectSource::Unknown)
        || source > static_cast<int>(AffectSource::Memory)
        || agency < static_cast<int>(AffectAgency::Unknown)
        || agency > static_cast<int>(AffectAgency::Environment)
        || outcome < static_cast<int>(AffectOutcome::Unknown)
        || outcome > static_cast<int>(AffectOutcome::Loss)) {
        return false;
    }
    if (!inSignedUnitRange(event.goalCongruence)
        || !inUnitRange(event.novelty)
        || !inUnitRange(event.certainty)
        || !inUnitRange(event.controllability)
        || !inUnitRange(event.relevance)
        || !inUnitRange(event.confidence)) {
        return false;
    }
    if (event.occurredAt.isValid()
        && now.msecsTo(event.occurredAt.toUTC()) > kMaxFutureEventSkewMs) {
        return false;
    }
    return true;
}

bool EmotionEngine::allowSource(const AffectEvent& event, qint64 nowMs) {
    QString sourceId = event.sourceId.trimmed();
    if (sourceId.isEmpty()) {
        sourceId = affectSourceToString(event.source);
    }
    const QString key = affectSourceToString(event.source) + QStringLiteral(":") + sourceId;
    QQueue<qint64>& timestamps = m_sourceEvents[key];
    const qint64 cutoff = nowMs - kSourceWindowMs;
    while (!timestamps.isEmpty() && timestamps.head() <= cutoff) {
        timestamps.dequeue();
    }
    if (timestamps.size() >= m_config.sameSourcePerMinute) {
        return false;
    }
    timestamps.enqueue(nowMs);
    return true;
}

void EmotionEngine::rememberEventId(const QString& id) {
    m_seenEventIds.insert(id);
    m_seenEventOrder.enqueue(id);
    while (m_seenEventOrder.size() > kSeenEventLimit) {
        m_seenEventIds.remove(m_seenEventOrder.dequeue());
    }
}

EmotionEngine::Evaluation EmotionEngine::evaluate(const AffectEvent& event) const {
    const double weight = event.relevance * event.confidence;
    const double positive = std::max(0.0, event.goalCongruence);
    const double negative = std::max(0.0, -event.goalCongruence);

    const double joy = positive * weight * (0.5 + 0.5 * event.certainty);
    const double lossFactor = event.outcome == AffectOutcome::Loss
        || event.kind == AffectEventKind::ConfirmedLoss
        ? 0.5 + 0.5 * (1.0 - event.controllability)
        : 0.25;
    const double sadness = negative * weight * lossFactor * event.certainty;

    const bool externalAgency = event.agency == AffectAgency::User
        || event.agency == AffectAgency::System
        || event.agency == AffectAgency::Environment;
    double anger = negative * weight * (externalAgency ? event.controllability : 0.0);
    if (event.agency == AffectAgency::User) {
        anger *= 0.75;
    }
    anger *= event.certainty;
    if (event.source == AffectSource::Tool) {
        anger *= 0.25;
    }

    double fear = negative * weight * (1.0 - event.controllability)
        * (0.5 + 0.5 * (1.0 - event.certainty));
    if (event.source == AffectSource::Tool) {
        fear *= 0.35;
    }
    const double surprise = event.novelty * weight;

    Evaluation result;
    const std::array<std::pair<EmotionType, double>, 5> candidates{{
        {EmotionType::Joy, joy},
        {EmotionType::Sadness, sadness},
        {EmotionType::Anger, anger},
        {EmotionType::Fear, fear},
        {EmotionType::Surprise, surprise}
    }};
    for (const auto& [emotion, score] : candidates) {
        if (score > result.score) {
            result.emotion = emotion;
            result.score = score;
        }
    }

    const double threshold = isNegativeEmotion(result.emotion)
        ? m_config.negativeThreshold
        : m_config.positiveThreshold;
    if (result.score < threshold) {
        result.emotion = EmotionType::Neutral;
        result.score = 0.0;
    }

    const double rawValence = event.goalCongruence * weight;
    const double negativeScale = rawValence < 0.0 ? m_config.negativeMultiplier : 1.0;
    result.valenceImpulse = std::clamp(
        rawValence * negativeScale,
        -m_config.maxValenceImpulse,
        m_config.maxValenceImpulse);
    result.arousalImpulse = std::clamp(
        std::max(std::abs(event.goalCongruence), event.novelty) * weight,
        0.0,
        m_config.maxArousalImpulse);
    return result;
}

bool EmotionEngine::advanceStateTo(const QDateTime& nowUtc) {
    if (!nowUtc.isValid()) {
        return false;
    }
    const QDateTime now = nowUtc.toUTC();
    if (!m_state.updatedAt.isValid()) {
        m_state.updatedAt = now;
        return true;
    }
    if (now <= m_state.updatedAt) {
        return false;
    }

    const double elapsedSec = m_state.updatedAt.msecsTo(now) / 1000.0;
    m_state.moodValence = std::clamp(
        decayToward(m_state.moodValence,
                    m_config.baselineValence,
                    elapsedSec,
                    m_config.valenceHalfLifeSec),
        -1.0,
        1.0);
    m_state.moodArousal = std::clamp(
        decayToward(m_state.moodArousal,
                    m_config.baselineArousal,
                    elapsedSec,
                    m_config.arousalHalfLifeSec),
        0.0,
        1.0);
    if (m_state.expressionExpiresAt.isValid() && now >= m_state.expressionExpiresAt) {
        m_state.active = EmotionType::Neutral;
        m_state.intensity = 0.0;
        m_state.confidence = 1.0;
        m_state.sourceEventId.clear();
        m_state.expressionExpiresAt = {};
    }
    m_state.updatedAt = now;
    return true;
}

bool EmotionEngine::applyEvaluation(const Evaluation& evaluation,
                                    const AffectEvent& event,
                                    const QDateTime& nowUtc,
                                    ExpressionRequest* request) {
    if (evaluation.emotion == EmotionType::Neutral) {
        return false;
    }

    bool accepted = false;
    if (m_state.active == EmotionType::Neutral || m_state.active == evaluation.emotion) {
        accepted = true;
    } else if (evaluation.score >= m_state.intensity + m_config.switchMargin) {
        accepted = true;
    }
    if (!accepted) {
        return false;
    }

    m_state.active = evaluation.emotion;
    m_state.intensity = std::max(m_state.intensity, evaluation.score);
    m_state.confidence = event.confidence;
    m_state.sourceEventId = event.id.trimmed();
    const QDateTime expiresAt = nowUtc.addMSecs(m_config.expressionDurationMs);
    if (!m_state.expressionExpiresAt.isValid() || expiresAt > m_state.expressionExpiresAt) {
        m_state.expressionExpiresAt = expiresAt;
    }

    if (evaluation.score < m_config.minExpressionIntensity || !request) {
        return false;
    }
    const int index = emotionIndex(evaluation.emotion);
    const qint64 nowMs = nowUtc.toMSecsSinceEpoch();
    const qint64 lastMs = m_lastExpressionAtMs.at(index);
    if (lastMs != std::numeric_limits<qint64>::min()
        && nowMs - lastMs < m_config.expressionCooldownMs) {
        return false;
    }

    m_lastExpressionAtMs[index] = nowMs;
    request->emotion = evaluation.emotion;
    request->intensity = evaluation.score;
    request->requestedAt = nowUtc;
    request->expiresAt = expiresAt;
    request->allowUnsolicited = !isNegativeEmotion(evaluation.emotion);
    return true;
}

void EmotionEngine::persist() {
    if (!m_repository) {
        return;
    }
    const PersistedEmotionState stored{
        1,
        m_state.moodValence,
        m_state.moodArousal,
        m_state.updatedAt.toUTC(),
        m_config.personalityRevision
    };
    if (!m_repository->save(stored)) {
        emit persistenceFailed(QStringLiteral("save"));
    }
}

EmotionSnapshot EmotionEngine::baselineSnapshot(const QDateTime& nowUtc) const {
    EmotionSnapshot state;
    state.moodValence = m_config.baselineValence;
    state.moodArousal = m_config.baselineArousal;
    state.active = EmotionType::Neutral;
    state.intensity = 0.0;
    state.confidence = 1.0;
    state.updatedAt = nowUtc.isValid() ? nowUtc.toUTC() : QDateTime{};
    return state;
}
