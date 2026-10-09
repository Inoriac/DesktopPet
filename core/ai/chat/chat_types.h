#ifndef DESKTOP_PET_CHAT_TYPES_H
#define DESKTOP_PET_CHAT_TYPES_H

#include <QDateTime>
#include <QString>
#include <QStringList>

#include <optional>

#include "ai/llm/llm_stream_types.h"

struct ChatHistoryEntry {
    QString id;
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

#endif // DESKTOP_PET_CHAT_TYPES_H
