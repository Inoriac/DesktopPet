#ifndef DESKTOP_PET_CHAT_PREPARATION_EXECUTOR_H
#define DESKTOP_PET_CHAT_PREPARATION_EXECUTOR_H

#include <QObject>
#include <QThread>

#include <atomic>
#include <memory>
#include <optional>
#ifdef DESKTOP_PET_ENABLE_TEST_SEAMS
#include <functional>
#endif

#include "chat_preparation_types.h"

class ChatPreparationExecutor final : public QObject {
    Q_OBJECT

public:
    explicit ChatPreparationExecutor(QObject* parent = nullptr);
    ~ChatPreparationExecutor() override;

    Result<void, DomainError> start(const ChatPreparationEnvironment& environment);
    void submit(ChatPreparationRequest request);
    void stop();

#ifdef DESKTOP_PET_ENABLE_TEST_SEAMS
    void setTestPreparationDelayMs(int delayMs);
    void setTestResourceLifecycleProbe(
        std::function<void(const QString&, quintptr)> probe);
    bool isWorkerThreadRunningForTests() const;
#endif

signals:
    void prepared(ChatPreparationResult result);

private:
    class Worker;
    void finalizeStoppedWorker();
    void handleWorkerThreadFinished();
    void shutdownAndWait();

    QThread m_thread;
    Worker* m_worker = nullptr;
    bool m_accepting = false;
    bool m_stopping = false;
    QString m_memoryDatabasePath;
    std::optional<ChatPreparationEnvironment> m_pendingRestartEnvironment;
    std::optional<ChatPreparationRequest> m_pendingRequest;
    std::shared_ptr<std::atomic<quint64>> m_cancellationEpoch;
#ifdef DESKTOP_PET_ENABLE_TEST_SEAMS
    std::shared_ptr<std::atomic<int>> m_testPreparationDelayMs;
    std::function<void(const QString&, quintptr)> m_testResourceLifecycleProbe;
#endif
};

#endif // DESKTOP_PET_CHAT_PREPARATION_EXECUTOR_H
