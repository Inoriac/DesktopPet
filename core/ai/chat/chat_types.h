#ifndef DESKTOP_PET_CHAT_TYPES_H
#define DESKTOP_PET_CHAT_TYPES_H

#include <QDateTime>
#include <QString>
#include <QStringList>

#include <optional>
#include <functional>
#include <QList>

#include "ai/llm/llm_stream_types.h"
#include "conversation_types.h"

struct ChatHistoryEntry {
    QString id;
    QString conversationId;
    QString speakerId;
    std::optional<ContextSpec> context;
    std::optional<ReplyTarget> replyTarget;
    QString role;
    QString replyToId;
    QString content;
    QDateTime timestamp;
    ChatMessageStatus status = ChatMessageStatus::Complete;
    QString errorMessage;
    QStringList sourceMessageIds;
};

bool isTerminalChatMessageStatus(ChatMessageStatus status);
QString chatMessageStatusStorageName(ChatMessageStatus status);
std::optional<ChatMessageStatus> chatMessageStatusFromStorageName(
    const QString& value);

using ChatHistoryResult = Result<QJsonObject, DomainError>;
using ChatHistoryCompletion = std::function<void(ChatHistoryResult)>;

ChatHistoryResult normalizeChatHistoryQuery(const QJsonObject& request, bool search);
bool isVisibleChatHistoryEntry(const ChatHistoryEntry& entry);
QJsonObject chatHistoryEntryJson(const ChatHistoryEntry& entry);
QJsonObject chatHistoryPage(const QList<ChatHistoryEntry>& entries,
                           bool hasMoreBefore, bool hasMoreAfter,
                           const QJsonObject& request = {}, bool newestFirst = false);
ChatHistoryResult queryChatHistoryEntries(const QList<ChatHistoryEntry>& entries,
                                         const QJsonObject& request);

#endif // DESKTOP_PET_CHAT_TYPES_H
