#ifndef DESKTOP_PET_EMOTION_STATE_PROVIDER_H
#define DESKTOP_PET_EMOTION_STATE_PROVIDER_H

#include <QDateTime>
#include <QList>
#include <QString>

#include "emotion/emotion_types.h"

class EmotionEngine;

struct ProvidedEmotionSnapshot {
    int schemaVersion = 1;
    QString profileId;
    EmotionSnapshot value;
    bool neutralFallback = false;
};

class EmotionStateProvider {
public:
    virtual ~EmotionStateProvider() = default;

    virtual ProvidedEmotionSnapshot currentSnapshot(
        const QString& profileId, const QDateTime& at) const = 0;
    virtual QList<ProvidedEmotionSnapshot> trajectory(
        const QString& profileId,
        const QDateTime& from,
        const QDateTime& to) const = 0;
};

class NullEmotionStateProvider final : public EmotionStateProvider {
public:
    ProvidedEmotionSnapshot currentSnapshot(
        const QString& profileId, const QDateTime& at) const override;
    QList<ProvidedEmotionSnapshot> trajectory(
        const QString& profileId,
        const QDateTime& from,
        const QDateTime& to) const override;
};

class EmotionEngineStateProvider final : public EmotionStateProvider {
public:
    explicit EmotionEngineStateProvider(const EmotionEngine* engine);

    ProvidedEmotionSnapshot currentSnapshot(
        const QString& profileId, const QDateTime& at) const override;
    QList<ProvidedEmotionSnapshot> trajectory(
        const QString& profileId,
        const QDateTime& from,
        const QDateTime& to) const override;

private:
    const EmotionEngine* m_engine = nullptr;
};

#endif // DESKTOP_PET_EMOTION_STATE_PROVIDER_H
