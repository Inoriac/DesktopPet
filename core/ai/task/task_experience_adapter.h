#pragma once

#include <QObject>
#include <QJsonArray>

#include "ai/reflection/cancellation_token.h"

class EventLedger;
class InnerThoughtService;
class MemoryStore;
class TaskStore;

// Retries durable task intents independently of result delivery. Only factual
// summaries reach the event/memory stores; generated feelings stay encrypted.
class TaskExperienceAdapter final : public QObject {
    Q_OBJECT
public:
    TaskExperienceAdapter(QString profileId, TaskStore* taskStore,
                          EventLedger* eventLedger, MemoryStore* memoryStore,
                          InnerThoughtService* innerThoughtService,
                          QObject* parent = nullptr);
    ~TaskExperienceAdapter() override;
    bool busy() const { return m_busy; }

public slots:
    void drain();
    void cancel();

signals:
    void experienceStored(const QString& taskId);
    void failed(const QString& taskId, const QString& errorCode);
    void drained();

private:
    void processNext();
    void advance();
    QString m_profileId;
    TaskStore* m_taskStore = nullptr;
    EventLedger* m_eventLedger = nullptr;
    MemoryStore* m_memoryStore = nullptr;
    InnerThoughtService* m_innerThoughtService = nullptr;
    CancellationSource m_cancellation;
    QJsonArray m_pending;
    qsizetype m_index = 0;
    bool m_busy = false;
};
