#ifndef DESKTOP_PET_MEMORY_METADATA_H
#define DESKTOP_PET_MEMORY_METADATA_H

#include <algorithm>
#include <QJsonArray>
#include <QSet>

#include "memory_types.h"
#include "recall_text.h"

// Routing labels remain readable for legacy filters, but are never topic evidence.
// New extractors record their exact routing labels in payload.source_tags as well.
namespace MemoryMetadata {
inline bool isSourceTag(const QString& value) {
    static const QSet<QString> labels = {
        "daydream_inbox", "user_interaction", "manual", "user_request",
        "user", "self", "working", "short_term", "episodic", "semantic",
        "preference", "procedural", "task_shadow", "relationship", "core", "event",
        "tool_result", "llm", "router", "proactive_chat", "touch_event",
        "idle_action", "emotion", "screen_observation"
    };
    return labels.contains(RecallText::normalize(value));
}

inline QStringList semanticTags(const MemoryEntry& entry) {
    QSet<QString> sourceTags;
    for (const auto& value : entry.payload.value("source_tags").toArray())
        sourceTags.insert(RecallText::normalize(value.toString()));
    QStringList tags;
    for (const auto& raw : entry.tags) {
        const QString tag = RecallText::normalize(raw);
        if (!tag.isEmpty() && !isSourceTag(tag) && !sourceTags.contains(tag)
            && !tags.contains(tag)) tags.append(tag);
    }
    return tags;
}

inline QStringList sessionIds(const MemoryEntry& entry) {
    QStringList ids;
    for (const auto& value : entry.payload.value("session_ids").toArray()) {
        const QString id = value.toString().trimmed();
        if (!id.isEmpty() && !ids.contains(id)) ids.append(id);
    }
    const QString latest = entry.payload.value("session_id").toString().trimmed();
    if (!latest.isEmpty() && !ids.contains(latest)) ids.append(latest);
    return ids;
}

inline void recordSession(MemoryEntry& entry, const QString& sessionId) {
    const QString id = sessionId.trimmed();
    if (id.isEmpty()) return;
    auto ids = sessionIds(entry);
    ids.removeAll(id);
    ids.append(id);
    // Bounded provenance for frequently repeated facts; latest context stays explicit.
    while (ids.size() > 32) ids.removeFirst();
    entry.payload["session_id"] = id;
    entry.payload["session_ids"] = QJsonArray::fromStringList(ids);
}

inline QDateTime lastMentionTime(const MemoryEntry& entry) {
    return entry.lastMentionedAt.isValid() ? entry.lastMentionedAt : entry.createdAt;
}

inline void mergeContext(MemoryEntry& target, const MemoryEntry& source) {
    const auto latest = std::max(lastMentionTime(target), lastMentionTime(source));
    if (latest.isValid()) target.lastMentionedAt = latest;
    for (const auto& id : sessionIds(source)) recordSession(target, id);
    // Copy only structural identifiers, not raw tool results or extraction defaults.
    for (const QString key : {QStringLiteral("task"), QStringLiteral("tool"),
                              QStringLiteral("event_id"), QStringLiteral("request_id")}) {
        const QString value = source.payload.value(key).toString().trimmed();
        if (!value.isEmpty()) target.payload[key] = value;
    }
}

inline QString contextHint(const MemoryEntry& entry) {
    const auto sessions = sessionIds(entry);
    if (!sessions.isEmpty()) return QStringLiteral("session:") + sessions.last();
    for (const QString key : {QStringLiteral("task"), QStringLiteral("tool")}) {
        const auto value = entry.payload.value(key).toString().trimmed();
        if (!value.isEmpty()) return key + QLatin1Char(':') + value;
    }
    return {};
}

inline bool shareContext(const MemoryEntry& a, const MemoryEntry& b) {
    const auto left = sessionIds(a), right = sessionIds(b);
    if (!left.isEmpty() && !right.isEmpty()) {
        for (const auto& id : left) if (right.contains(id)) return true;
        return false;
    }
    const auto hint = contextHint(a);
    return !hint.isEmpty() && hint == contextHint(b);
}

// Available before classification, including Chinese text without spaces.
inline double textSimilarity(const MemoryEntry& a, const MemoryEntry& b) {
    const QString left = a.summary + QLatin1Char(' ') + a.content;
    const QString right = b.summary + QLatin1Char(' ') + b.content;
    const auto aTokens = RecallText::tokens(left), bTokens = RecallText::tokens(right);
    const QSet<QString> aSet(aTokens.cbegin(), aTokens.cend()), bSet(bTokens.cbegin(), bTokens.cend());
    return (RecallText::lexicalCoverage(left, aTokens, bSet)
          + RecallText::lexicalCoverage(right, bTokens, aSet)) / 2.0;
}
}
#endif
