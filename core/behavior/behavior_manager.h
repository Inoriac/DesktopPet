#pragma once
#include "emotion/emotion_types.h"
#include "presentation/avatar_presentation.h"
#include <QDateTime>
#include <QHash>
#include <QObject>

enum class ExpressionDisposition { Played, Queued, Dropped };

// Emotion chooses an intent; the shared avatar runtime owns all animation
// queues, mappings and protection. No parallel Idle-only queue exists here.
class BehaviorManager : public QObject {
    Q_OBJECT
  public:
    explicit BehaviorManager(int queueLimit = 3, QObject* parent = nullptr);
    void setPresentation(AvatarPresentation* presentation);
    ExpressionDisposition handleExpression(const ExpressionRequest& request, const QDateTime& nowUtc);
    void clearPending();
    int pendingCount() const {
        return m_pending.size();
    }
  signals:
    void expressionPlayed(ExpressionRequest request, QString state);
    void expressionQueued(ExpressionRequest request);
    void expressionDropped(ExpressionRequest request, QString reason);

  private:
    int m_queueLimit = 3;
    QPointer<AvatarPresentation> m_presentation;
    QHash<QString, ExpressionRequest> m_pending;
};
