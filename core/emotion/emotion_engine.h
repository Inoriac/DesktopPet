#ifndef DESKTOP_PET_EMOTION_ENGINE_H
#define DESKTOP_PET_EMOTION_ENGINE_H

#include "emotion_state_repository.h"
#include "emotion_types.h"

#include <QHash>
#include <QObject>
#include <QQueue>
#include <QSet>

#include <array>

class EmotionEngine : public QObject {
    Q_OBJECT

public:
    explicit EmotionEngine(EmotionConfig config = {},
                           IEmotionStateRepository* repository = nullptr,
                           QObject* parent = nullptr);

    const EmotionConfig& config() const { return m_config; }
    bool isEnabled() const { return m_enabled; }

    bool submitEvent(const AffectEvent& event, const QDateTime& nowUtc);
    void advanceTo(const QDateTime& nowUtc);
    EmotionSnapshot snapshot(const QDateTime& nowUtc = {}) const;
    bool restore(const QDateTime& nowUtc);
    void reset(const QDateTime& nowUtc);
    void setEnabled(bool enabled, const QDateTime& nowUtc);

signals:
    void stateChanged(EmotionSnapshot snapshot);
    void expressionRequested(ExpressionRequest request);
    void persistenceFailed(QString operation);

private:
    struct Evaluation {
        EmotionType emotion = EmotionType::Neutral;
        double score = 0.0;
        double valenceImpulse = 0.0;
        double arousalImpulse = 0.0;
    };

    bool validateEvent(const AffectEvent& event, const QDateTime& nowUtc) const;
    bool allowSource(const AffectEvent& event, qint64 nowMs);
    void rememberEventId(const QString& id);
    Evaluation evaluate(const AffectEvent& event) const;
    bool advanceStateTo(const QDateTime& nowUtc);
    bool applyEvaluation(const Evaluation& evaluation,
                         const AffectEvent& event,
                         const QDateTime& nowUtc,
                         ExpressionRequest* request);
    void persist();
    EmotionSnapshot baselineSnapshot(const QDateTime& nowUtc) const;

    EmotionConfig m_config;
    IEmotionStateRepository* m_repository = nullptr;
    bool m_enabled = true;
    EmotionSnapshot m_state;
    QSet<QString> m_seenEventIds;
    QQueue<QString> m_seenEventOrder;
    QHash<QString, QQueue<qint64>> m_sourceEvents;
    std::array<qint64, 6> m_lastExpressionAtMs{};
};

#endif // DESKTOP_PET_EMOTION_ENGINE_H
