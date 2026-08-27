#include "chat_types.h"

bool isTerminalChatMessageStatus(ChatMessageStatus status) {
    return status == ChatMessageStatus::Complete
        || status == ChatMessageStatus::Interrupted
        || status == ChatMessageStatus::Stopped
        || status == ChatMessageStatus::Failed;
}

QString chatMessageStatusStorageName(ChatMessageStatus status) {
    switch (status) {
    case ChatMessageStatus::Complete:
        return QStringLiteral("complete");
    case ChatMessageStatus::Interrupted:
        return QStringLiteral("interrupted");
    case ChatMessageStatus::Stopped:
        return QStringLiteral("stopped");
    case ChatMessageStatus::Failed:
        return QStringLiteral("failed");
    case ChatMessageStatus::Pending:
    case ChatMessageStatus::Streaming:
        return {};
    }
    return {};
}

std::optional<ChatMessageStatus> chatMessageStatusFromStorageName(
    const QString& value) {
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("complete")) return ChatMessageStatus::Complete;
    if (normalized == QStringLiteral("interrupted")) return ChatMessageStatus::Interrupted;
    if (normalized == QStringLiteral("stopped")) return ChatMessageStatus::Stopped;
    if (normalized == QStringLiteral("failed")) return ChatMessageStatus::Failed;
    return std::nullopt;
}
