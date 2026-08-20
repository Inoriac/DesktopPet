#ifndef DESKTOP_PET_PET_CONTROLLER_H
#define DESKTOP_PET_PET_CONTROLLER_H

#include "emotion/emotion_engine.h"

#include <QDateTime>
#include <QObject>
#include <QString>

class PetController : public QObject {
    Q_OBJECT

public:
    explicit PetController(EmotionEngine* emotionEngine, QObject* parent = nullptr);

    bool recordTouch(const QString& part,
                     const QDateTime& nowUtc = QDateTime::currentDateTimeUtc(),
                     const QString& eventId = {});
    bool recordToolOutcome(const QString& toolName,
                           bool success,
                           const QDateTime& nowUtc = QDateTime::currentDateTimeUtc(),
                           const QString& eventId = {});
    bool recordTaskOutcome(const QString& taskId,
                           bool success,
                           const QDateTime& nowUtc = QDateTime::currentDateTimeUtc(),
                           const QString& eventId = {});
    bool recordExplicitFeedbackText(const QString& text,
                                    const QDateTime& nowUtc = QDateTime::currentDateTimeUtc(),
                                    const QString& eventId = {});

signals:
    void affectEventAccepted(QString eventId, AffectEventKind kind);
    void affectEventIgnored(QString eventId, AffectEventKind kind);

private:
    bool submit(AffectEvent event, const QDateTime& nowUtc, const QString& eventId);
    static QString makeEventId(const QString& prefix, const QString& suppliedId);

    EmotionEngine* m_emotionEngine = nullptr;
};

#endif // DESKTOP_PET_PET_CONTROLLER_H
