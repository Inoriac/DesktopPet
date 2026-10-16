#ifndef DESKTOP_PET_CONVERSATION_SERVICE_H
#define DESKTOP_PET_CONVERSATION_SERVICE_H
#include "conversation_store.h"
#include <QObject>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>
#include <QMetaObject>
#include <atomic>

// Serial storage lane. Dispatchers are entered on this QObject's owner thread;
// their contract is asynchronous and never executes blocking tools on that thread.
class ConversationService final : public QObject {
    Q_OBJECT
public:
    template<class T> using Completion=std::function<void(Result<T,DomainError>)>;
    ConversationService(QString profileId, QString databasePath, QObject* parent=nullptr);
    ~ConversationService() override;
    Result<void,DomainError> registerBinding(const HostConversationBinding& binding);
    void acceptMessageAsync(IncomingMessage message, Completion<AcceptedMessage> completion);
    void prepareContextAsync(ContextRequestV2 request, Completion<PreparedContext> completion);
    void publishResponseAsync(RoutedResponse response, Completion<void> completion);
    void recordDeliveryReceiptAsync(DeliveryReceipt receipt, Completion<void> completion);
    void appendLocalHistoryAsync(ChatHistoryEntry entry, Completion<void> completion);
    void cancelRequestAsync(QString requestId, quint64 generation);
    ConversationStore storeSnapshot() const { return m_store; }
signals:
    void replyDispatchPrepared(const QString& requestId, quint64 generation);
private:
    template<class T, class Operation>
    void enqueue(Operation operation, Completion<T> completion) {
        QPointer<ConversationService> guard(this);
        auto store=m_store;
        const auto ready=m_storageReady;
        m_worker.start([guard,ready,store=std::move(store),operation=std::move(operation),completion=std::move(completion)]() mutable {
            auto initialized=ready->load() ? Result<void,DomainError>::success() : store.initialize();
            if (initialized) ready->store(true);
            Result<T,DomainError> result=initialized ? operation(store) : Result<T,DomainError>::failure(initialized.error());
            if (!guard) return;
            QMetaObject::invokeMethod(guard,[guard,result=std::move(result),completion=std::move(completion)]() mutable {
                if (!guard) return;
                if (!guard->m_outboxRetryTimer.isActive()) guard->m_outboxRetryTimer.start();
                if (completion) completion(std::move(result));
            },Qt::QueuedConnection);
        });
    }
    QHash<QString,std::shared_ptr<ToolDispatchToken>> m_dispatchTokens;
    std::shared_ptr<std::atomic_bool> m_storageReady = std::make_shared<std::atomic_bool>(false);
    ConversationStore m_store;
    QHash<QString,HostConversationBinding> m_bindings;
    QThreadPool m_worker;
    QTimer m_outboxRetryTimer;
    bool m_recoveringOutbox = false;
};
#endif
