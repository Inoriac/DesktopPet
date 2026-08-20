#ifndef DESKTOP_PET_BEHAVIOR_MANAGER_H
#define DESKTOP_PET_BEHAVIOR_MANAGER_H

#include "emotion/emotion_types.h"

#include <QDateTime>
#include <QObject>
#include <QQueue>
#include <QString>

#include <functional>

enum class ExpressionDisposition {
    Played,
    Queued,
    Dropped
};

class BehaviorManager : public QObject {
    Q_OBJECT

public:
    using CurrentStateProvider = std::function<QString()>;
    using AnimationPlayer = std::function<bool(const QString&)>;

    explicit BehaviorManager(int queueLimit = 3, QObject* parent = nullptr);

    void setCurrentStateProvider(CurrentStateProvider provider);
    void setAnimationPlayer(AnimationPlayer player);

    ExpressionDisposition handleExpression(const ExpressionRequest& request,
                                            const QDateTime& nowUtc);
    void processPending(const QDateTime& nowUtc);
    void clearPending();
    int pendingCount() const { return m_pending.size(); }

    static QString animationStateFor(EmotionType emotion);

signals:
    void expressionPlayed(ExpressionRequest request, QString animationState);
    void expressionQueued(ExpressionRequest request);
    void expressionDropped(ExpressionRequest request, QString reason);

private:
    bool canPlayNow() const;
    bool play(const ExpressionRequest& request);
    void enqueue(const ExpressionRequest& request);

    int m_queueLimit = 3;
    QQueue<ExpressionRequest> m_pending;
    CurrentStateProvider m_currentStateProvider;
    AnimationPlayer m_animationPlayer;
};

#endif // DESKTOP_PET_BEHAVIOR_MANAGER_H
