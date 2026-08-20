#include "behavior_manager.h"

#include <algorithm>
#include <cmath>
#include <utility>

BehaviorManager::BehaviorManager(int queueLimit, QObject* parent)
    : QObject(parent),
      m_queueLimit(std::clamp(queueLimit, 0, 32)) {}

void BehaviorManager::setCurrentStateProvider(CurrentStateProvider provider) {
    m_currentStateProvider = std::move(provider);
}

void BehaviorManager::setAnimationPlayer(AnimationPlayer player) {
    m_animationPlayer = std::move(player);
}

ExpressionDisposition BehaviorManager::handleExpression(const ExpressionRequest& request,
                                                        const QDateTime& nowUtc) {
    if (!nowUtc.isValid()
        || request.emotion == EmotionType::Neutral
        || !std::isfinite(request.intensity)
        || request.intensity < 0.0
        || request.intensity > 1.0
        || !request.expiresAt.isValid()
        || request.expiresAt.toUTC() <= nowUtc.toUTC()) {
        emit expressionDropped(request, QStringLiteral("invalid_or_expired"));
        return ExpressionDisposition::Dropped;
    }
    if (animationStateFor(request.emotion).isEmpty()) {
        emit expressionDropped(request, QStringLiteral("unsupported_emotion"));
        return ExpressionDisposition::Dropped;
    }
    if (canPlayNow()) {
        if (play(request)) {
            return ExpressionDisposition::Played;
        }
        emit expressionDropped(request, QStringLiteral("animation_unavailable"));
        return ExpressionDisposition::Dropped;
    }
    if (m_queueLimit <= 0) {
        emit expressionDropped(request, QStringLiteral("busy"));
        return ExpressionDisposition::Dropped;
    }
    enqueue(request);
    emit expressionQueued(request);
    return ExpressionDisposition::Queued;
}

void BehaviorManager::processPending(const QDateTime& nowUtc) {
    if (!nowUtc.isValid()) {
        return;
    }
    const QDateTime now = nowUtc.toUTC();
    while (!m_pending.isEmpty() && m_pending.head().expiresAt.toUTC() <= now) {
        emit expressionDropped(m_pending.dequeue(), QStringLiteral("expired"));
    }
    if (m_pending.isEmpty() || !canPlayNow()) {
        return;
    }

    const ExpressionRequest request = m_pending.dequeue();
    if (!play(request)) {
        emit expressionDropped(request, QStringLiteral("animation_unavailable"));
    }
}

void BehaviorManager::clearPending() {
    m_pending.clear();
}

QString BehaviorManager::animationStateFor(EmotionType emotion) {
    switch (emotion) {
    case EmotionType::Joy:
        return QStringLiteral("Happy");
    case EmotionType::Sadness:
        return QStringLiteral("Cry");
    case EmotionType::Anger:
        return QStringLiteral("Angry");
    case EmotionType::Fear:
        return QStringLiteral("Fear");
    case EmotionType::Neutral:
    case EmotionType::Surprise:
        return {};
    }
    return {};
}

bool BehaviorManager::canPlayNow() const {
    if (!m_currentStateProvider || !m_animationPlayer) {
        return false;
    }
    return m_currentStateProvider().compare(QStringLiteral("Idle"), Qt::CaseInsensitive) == 0;
}

bool BehaviorManager::play(const ExpressionRequest& request) {
    const QString animationState = animationStateFor(request.emotion);
    if (animationState.isEmpty() || !m_animationPlayer || !m_animationPlayer(animationState)) {
        return false;
    }
    emit expressionPlayed(request, animationState);
    return true;
}

void BehaviorManager::enqueue(const ExpressionRequest& request) {
    while (m_pending.size() >= m_queueLimit) {
        emit expressionDropped(m_pending.dequeue(), QStringLiteral("queue_full"));
    }
    m_pending.enqueue(request);
}
