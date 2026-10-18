#ifndef DESKTOP_PET_CHAT_CONVERSATION_MODEL_H
#define DESKTOP_PET_CHAT_CONVERSATION_MODEL_H

#include "ai/chat/profile_chat_history_store.h"

#include <QHash>
#include <QObject>
#include <QThreadPool>

class ChatConversationModel : public QObject {
    Q_OBJECT

public:
    explicit ChatConversationModel(QObject* parent = nullptr);
    ~ChatConversationModel() override;

    void setDeferredPersistence(bool enabled);
    bool initialize(const ProfileChatStoreOptions& options,
                    QString* errorMessage);
    QString appendUserMessage(
        const QString& text,
        const QDateTime& timestamp = QDateTime::currentDateTime(),
        const QJsonArray& images = {});
    void appendUserMessageWithImages(const QString& text, const QJsonArray& sources,
                                     ChatHistoryCompletion completion);
    QJsonArray sourceImagesForReply(const QString& assistantMessageId) const;
    void beginAssistantMessage(
        const QString& messageId,
        const QString& replyToId = {},
        const QDateTime& timestamp = QDateTime::currentDateTime(),
        const QStringList& sourceMessageIds = {});
    QString sourceInputForReply(const QString& assistantMessageId) const;
    void appendAssistantDelta(const QString& messageId, const QString& delta);
    void setAssistantStage(const QString& messageId, ChatActivityStage stage);
    void finishAssistantMessage(const QString& messageId,
                                ChatMessageStatus status,
                                const QString& errorMessage = {});
    void markReadThrough(const QString& messageId);
    QString lastReadMessageId() const;
    QList<ChatHistoryEntry> messages() const;
    QJsonObject recentHistoryPage(int limit = 40) const;
    void requestHistory(const QString& action, const QJsonObject& request,
                        ChatHistoryCompletion completion);

signals:
    void messageInserted(int index, const QString& messageId);
    void messageChanged(int index, const QString& messageId);
    void assistantStageChanged(const QString& messageId, ChatActivityStage stage);
    void historyPersistenceWarning(const QString& safeMessage);

private:
    int indexOf(const QString& messageId) const;
    bool appendToStore(const ChatHistoryEntry& entry);
    void warnUnknownMessage();
    QString readMarkerKey() const;

    ProfileChatHistoryStore m_store;
    QList<ChatHistoryEntry> m_messages;
    QHash<QString, int> m_messageIndexes;
    QString m_profileId;
    QString m_imageDirectory;
    QString m_lastReadMessageId;
    bool m_initialized = false;
    bool m_persistenceAvailable = false;
    bool m_deferredPersistence = false;
    QThreadPool m_persistencePool;
    quint64 m_historyGeneration = 0;
};

#endif // DESKTOP_PET_CHAT_CONVERSATION_MODEL_H
