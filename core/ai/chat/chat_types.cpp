#include "chat_types.h"
#include <QJsonArray>
#include <QSet>
#include <algorithm>

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


ChatHistoryResult normalizeChatHistoryQuery(const QJsonObject& request, bool search) {
    const auto invalid = []() {
        return ChatHistoryResult::failure(domainError("CHAT_QUERY_INVALID",
            QStringLiteral("聊天记录查询条件无效，请刷新后重试。")));
    };
    static const QSet<QString> fields{"beforeMessageId", "aroundMessageId", "query", "limit", "requestToken"};
    for (auto it = request.constBegin(); it != request.constEnd(); ++it)
        if (!fields.contains(it.key())) return invalid();
    for (const auto& field : {"beforeMessageId", "aroundMessageId", "requestToken"}) {
        if (request.contains(field) && !request[field].isString()) return invalid();
    }
    const QString before = request["beforeMessageId"].toString();
    const QString around = request["aroundMessageId"].toString();
    if (!around.isEmpty() && (!before.isEmpty() || search)) return invalid();
    const int limit = request["limit"].toInt(search ? 30 : 40);
    if (request.contains("limit") && (!request["limit"].isDouble()
        || request["limit"].toDouble() != limit)) return invalid();
    if (limit < 1 || limit > (search ? 50 : 80)) return invalid();
    QJsonObject normalized{{"beforeMessageId", before}, {"aroundMessageId", around},
        {"limit", limit}, {"requestToken", request["requestToken"].toString()}};
    if (search) {
        const QString query = request["query"].toString().trimmed();
        if (query.isEmpty() || query.size() > 200) return invalid();
        normalized["query"] = query;
    } else if (request.contains("query")) return invalid();
    return ChatHistoryResult::success(normalized);
}

bool isVisibleChatHistoryEntry(const ChatHistoryEntry& entry) {
    return (entry.role == "user" || entry.role == "assistant")
        && (!entry.content.trimmed().isEmpty() || !entry.images.isEmpty() || entry.status == ChatMessageStatus::Pending
            || entry.status == ChatMessageStatus::Streaming
            || (entry.role == "assistant" && !entry.replyToId.isEmpty()
                && (entry.status == ChatMessageStatus::Failed
                    || entry.status == ChatMessageStatus::Interrupted
                    || entry.status == ChatMessageStatus::Stopped)));
}

QJsonObject chatHistoryEntryJson(const ChatHistoryEntry& entry) {
    const QString status = entry.status == ChatMessageStatus::Pending ? QString("pending")
        : entry.status == ChatMessageStatus::Streaming ? QString("streaming")
        : chatMessageStatusStorageName(entry.status);
    return {{"id", entry.id}, {"role", entry.role}, {"replyToId", entry.replyToId},
        {"content", entry.content}, {"timestamp", entry.timestamp.toString(Qt::ISODateWithMs)},
        {"status", status}, {"errorMessage", entry.errorMessage}, {"images",entry.images}};
}

QJsonObject chatHistoryPage(const QList<ChatHistoryEntry>& entries,
                           bool hasMoreBefore, bool hasMoreAfter,
                           const QJsonObject& request, bool newestFirst) {
    QJsonArray messages;
    for (const auto& entry : entries) messages.append(chatHistoryEntryJson(entry));
    const QString first = entries.isEmpty() ? QString() : entries.first().id;
    const QString last = entries.isEmpty() ? QString() : entries.last().id;
    QJsonObject page{{"messages", messages}, {"hasMoreBefore", hasMoreBefore},
        {"hasMoreAfter", hasMoreAfter}, {"oldestMessageId", newestFirst ? last : first},
        {"newestMessageId", newestFirst ? first : last},
        {"requestToken", request["requestToken"].toString()}};
    if (request.contains("query")) {
        page["query"] = request["query"];
        page["nextBeforeMessageId"] = last;
    }
    if (!request["aroundMessageId"].toString().isEmpty())
        page["anchorMessageId"] = request["aroundMessageId"];
    return page;
}

ChatHistoryResult queryChatHistoryEntries(const QList<ChatHistoryEntry>& allEntries,
                                         const QJsonObject& request) {
    const bool search = request.contains("query");
    const auto normalized = normalizeChatHistoryQuery(request, search);
    if (!normalized) return normalized;
    const auto& query = normalized.value();
    QList<ChatHistoryEntry> entries;
    for (const auto& entry : allEntries)
        if (isVisibleChatHistoryEntry(entry)) entries.append(entry);
    const int count = int(entries.size());
    const int limit = query["limit"].toInt();
    const QString before = query["beforeMessageId"].toString();
    const QString around = query["aroundMessageId"].toString();
    int cursor = count;
    const QString cursorId = around.isEmpty() ? before : around;
    if (!cursorId.isEmpty()) {
        cursor = -1;
        for (int i = 0; i < count; ++i) {
            if (entries[i].id == cursorId) { cursor = i; break; }
        }
        if (cursor < 0) return ChatHistoryResult::failure(domainError("CHAT_QUERY_INVALID",
            QStringLiteral("这条聊天记录已不可用，请刷新后重试。")));
    }
    if (search) {
        QList<ChatHistoryEntry> matches;
        const QString text = query["query"].toString();
        for (int i = cursor - 1; i >= 0 && matches.size() <= limit; --i) {
            const auto& entry = entries[i];
            if ((entry.role == "user" || entry.role == "assistant")
                && isTerminalChatMessageStatus(entry.status)
                && !entry.content.trimmed().isEmpty()
                && entry.content.contains(text, Qt::CaseInsensitive)) matches.append(entry);
        }
        const bool more = matches.size() > limit;
        if (more) matches.removeLast();
        return ChatHistoryResult::success(chatHistoryPage(matches, more, false, query, true));
    }
    int end = cursor;
    int start = std::max(0, end - limit);
    if (!around.isEmpty()) {
        start = std::max(0, cursor - limit / 2);
        end = std::min(count, start + limit);
        start = std::max(0, end - limit);
    }
    return ChatHistoryResult::success(chatHistoryPage(entries.mid(start, end - start),
        start > 0, end < count, query));
}
