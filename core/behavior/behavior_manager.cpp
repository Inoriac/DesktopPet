#include "behavior_manager.h"
#include <algorithm>
#include <cmath>
BehaviorManager::BehaviorManager(int limit, QObject* parent)
    : QObject(parent), m_queueLimit(std::clamp(limit, 0, 32)) {}
void BehaviorManager::setPresentation(AvatarPresentation* presentation) {
    clearPending();
    if (m_presentation)
        disconnect(m_presentation, nullptr, this, nullptr);
    m_presentation = presentation;
    if (!presentation)
        return;
    connect(presentation, &AvatarPresentation::requestChanged, this, [this](const QJsonObject& result) {
        const auto id = result["request_id"].toString();
        if (!result["terminal"].toBool() || !m_pending.contains(id))
            return;
        const auto request = m_pending.take(id);
        if (result["status"] == "completed")
            emit expressionPlayed(request, result["current_state"].toString());
        else
            emit expressionDropped(request, result["reason"].toString());
    });
}
ExpressionDisposition BehaviorManager::handleExpression(const ExpressionRequest& request,
                                                        const QDateTime& now) {
    if (!now.isValid() || request.emotion == EmotionType::Neutral || !std::isfinite(request.intensity) ||
        request.intensity < 0 || request.intensity > 1 || !request.expiresAt.isValid() ||
        request.expiresAt <= now) {
        emit expressionDropped(request, "invalid_or_expired");
        return ExpressionDisposition::Dropped;
    }
    if (!m_presentation) {
        emit expressionDropped(request, "presentation_unavailable");
        return ExpressionDisposition::Dropped;
    }
    if (m_pending.size() >= m_queueLimit && m_presentation->snapshot()["protected"].toBool()) {
        emit expressionDropped(request, "queue_full");
        return ExpressionDisposition::Dropped;
    }
    const auto result = m_presentation->requestSemantic(
        "emotion." + emotionTypeToString(request.emotion).toLower(), AvatarRequestSource::Emotion,
        static_cast<int>(now.msecsTo(request.expiresAt)));
    if (result["status"] == "completed") {
        emit expressionPlayed(request, result["current_state"].toString());
        return ExpressionDisposition::Played;
    }
    if (result["terminal"].toBool()) {
        emit expressionDropped(request, result["reason"].toString());
        return ExpressionDisposition::Dropped;
    }
    m_pending.insert(result["request_id"].toString(), request);
    emit expressionQueued(request);
    return ExpressionDisposition::Queued;
}
void BehaviorManager::clearPending() {
    const auto ids = m_pending.keys();
    for (const auto& id : ids)
        if (m_presentation)
            m_presentation->cancel(id);
    m_pending.clear();
}
