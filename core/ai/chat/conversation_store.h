#ifndef DESKTOP_PET_CONVERSATION_STORE_H
#define DESKTOP_PET_CONVERSATION_STORE_H
#include "conversation_types.h"
#include "chat_types.h"
#include <QHash>

class RuntimeUnitOfWork;

// No QSqlDatabase survives a call. Copies may be used by independent workers.
class ConversationStore {
public:
    ConversationStore(QString profileId, QString databasePath);
    Result<void, DomainError> initialize();
    Result<void, DomainError> registerBinding(const HostConversationBinding& binding);
    using AcceptanceCommit = std::function<Result<void, DomainError>(
        RuntimeUnitOfWork &, AcceptedMessage &)>;
    Result<AcceptedMessage, DomainError>
    acceptMessage(const IncomingMessage &message,
                  const AcceptanceCommit &commit = {});
    Result<QList<ChatHistoryEntry>, DomainError> loadHistory(const QString& conversationId,
                                                           int limit = 10000) const;
    ChatHistoryResult queryLocalHistory(const QJsonObject& request) const;
    Result<void, DomainError> appendLocalHistory(const ChatHistoryEntry& entry);
    Result<void, DomainError> importLocalHistory(const QList<ChatHistoryEntry>& history,
                                                const QString& sourceRef);
    Result<bool, DomainError> localHistoryMigrationComplete() const;
    Result<PreparedContext, DomainError> prepareContext(const ContextRequestV2& request);
    Result<void, DomainError> publishResponse(const RoutedResponse& response);
    Result<void, DomainError> recordDeliveryReceipt(const DeliveryReceipt& receipt);
    Result<void, DomainError> cancelRequest(const QString& requestId, quint64 generation, bool beforeDispatch = false);
    Result<ContextSpec, DomainError> currentContext(const QString& conversationId) const;
    Result<void, DomainError> validateContext(const ContextSpec& context) const;
    Result<int, DomainError> recoverOutbox();
    const QString& profileId() const { return m_profileId; }
    const QString& databasePath() const { return m_databasePath; }
private:
    QString m_profileId;
    QString m_databasePath;
    QHash<QString, HostConversationBinding> m_bindings;
};
#endif
