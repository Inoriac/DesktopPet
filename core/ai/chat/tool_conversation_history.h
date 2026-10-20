#ifndef DESKTOP_PET_TOOL_CONVERSATION_HISTORY_H
#define DESKTOP_PET_TOOL_CONVERSATION_HISTORY_H

#include "ai/context/context_spec.h"
#include "ai_types.h"

#include <QHash>
#include <QJsonDocument>

// Process-local continuation material, separate from visible chat and long-term memory.
// Only complete assistant/tool rounds are retained or evicted.
class ToolConversationHistory {
public:
    static constexpr qsizetype MaxBytes = 32 * 1024;
    static constexpr int MaxRoundsPerConversation = 2;

    quint64 epoch() const { return m_epoch; }
    void clear() {
        m_rounds.clear();
        m_bytes = 0;
        ++m_epoch;
    }

    bool add(const ContextSpec& context, const QList<ChatMessage>& messages) {
        if (messages.isEmpty() || messages.first().role != "assistant"
            || messages.first().toolCalls.isEmpty()
            || messages.size() != messages.first().toolCalls.size() + 1) return false;
        QHash<QString, QString> pending;
        for (const auto& value : messages.first().toolCalls) {
            const auto call = value.toObject();
            const auto id = call["id"].toString();
            const auto name = call["function"].toObject()["name"].toString();
            if (id.isEmpty() || name.isEmpty() || pending.contains(id)) return false;
            pending.insert(id, name);
        }
        for (const auto& message : messages.sliced(1)) {
            const auto call = pending.constFind(message.toolCallId);
            if (message.role != "tool" || call == pending.constEnd()
                || message.name != *call || !message.toolCalls.isEmpty()) return false;
            pending.remove(message.toolCallId);
        }

        Round round;
        round.conversation = conversationKey(context);
        round.scope = scopeKey(context);
        QJsonArray serialized;
        for (const auto& message : messages) {
            ChatMessage stored;
            stored.role = message.role;
            stored.content = message.content;
            stored.name = message.name;
            stored.toolCallId = message.toolCallId;
            stored.toolCalls = message.toolCalls;
            serialized.append(QJsonObject{{"role", stored.role}, {"content", stored.content},
                {"name", stored.name}, {"toolCallId", stored.toolCallId}, {"toolCalls", stored.toolCalls}});
            round.messages.append(std::move(stored));
        }
        round.bytes = QJsonDocument(serialized).toJson(QJsonDocument::Compact).size()
            + round.conversation.size() + round.scope.size();
        if (round.bytes > MaxBytes) return false;

        // A changed audience or identity invalidates the earlier scope of this conversation.
        int retained = 0;
        for (qsizetype i = m_rounds.size(); i-- > 0;) {
            if (m_rounds[i].conversation != round.conversation) continue;
            if (m_rounds[i].scope != round.scope || ++retained >= MaxRoundsPerConversation)
                remove(i);
        }
        m_bytes += round.bytes;
        m_rounds.append(std::move(round));
        while (m_bytes > MaxBytes) remove(0);
        return true;
    }

    QList<ChatMessage> messages(const ContextSpec& context) const {
        QList<ChatMessage> result;
        const auto scope = scopeKey(context);
        for (const auto& round : m_rounds)
            if (round.scope == scope) result.append(round.messages);
        return result;
    }

private:
    struct Round {
        QByteArray conversation;
        QByteArray scope;
        QList<ChatMessage> messages;
        qsizetype bytes = 0;
    };
    static QByteArray conversationKey(const ContextSpec& context) {
        return QJsonDocument(QJsonArray{context.profileId, context.channel,
            context.conversationId, context.conversationId.isEmpty() ? context.activityId : QString(),
            int(context.originKind)}).toJson(QJsonDocument::Compact);
    }
    static QByteArray scopeKey(const ContextSpec& context) {
        auto participants = context.participantPersonIds;
        participants.removeDuplicates();
        participants.sort();
        // conversationRevision advances with every message, not just scope changes.
        return QJsonDocument(QJsonArray{context.cacheKey(), context.channel,
            context.identityRevision,
            QJsonArray::fromStringList(participants)}).toJson(QJsonDocument::Compact);
    }
    void remove(qsizetype index) {
        m_bytes -= m_rounds[index].bytes;
        m_rounds.removeAt(index);
    }
    QList<Round> m_rounds;
    qsizetype m_bytes = 0;
    quint64 m_epoch = 0;
};

#endif
