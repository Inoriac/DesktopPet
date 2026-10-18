#include "chat_conversation_model.h"

#include <QMetaObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSettings>
#include <QUuid>
#include <QJsonArray>

namespace {

bool canFinish(ChatMessageStatus current, ChatMessageStatus terminal) {
    if (terminal == ChatMessageStatus::Stopped) {
        return current == ChatMessageStatus::Pending
            || current == ChatMessageStatus::Streaming;
    }
    if (terminal == ChatMessageStatus::Failed) {
        return current == ChatMessageStatus::Pending;
    }
    if (terminal == ChatMessageStatus::Complete
        || terminal == ChatMessageStatus::Interrupted) {
        return current == ChatMessageStatus::Streaming;
    }
    return false;
}

QString sanitizedChatError(QString message) {
    message = message.simplified();
    if (message.isEmpty()) return {};

    static const QRegularExpression bearerPattern(
        QStringLiteral("\\b(Bearer\\s+)[^\\s,;]+"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression namedSecretPattern(
        QStringLiteral("\\b((?:x-)?api[-_ ]?key|authorization|access[-_ ]?token|token"
                       "\\s*[:=]\\s*)[^\\s,;&]+"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression querySecretPattern(
        QStringLiteral("([?&](?:api[-_]?key|access[-_]?token|token)=)[^&\\s]+"),
        QRegularExpression::CaseInsensitiveOption);
    message.replace(bearerPattern, QStringLiteral("\\1[REDACTED]"));
    message.replace(namedSecretPattern, QStringLiteral("\\1[REDACTED]"));
    message.replace(querySecretPattern, QStringLiteral("\\1[REDACTED]"));

    constexpr qsizetype kMaximumErrorCharacters = 360;
    if (message.size() > kMaximumErrorCharacters) {
        message = message.left(kMaximumErrorCharacters) + QStringLiteral("...");
    }
    return message;
}

} // namespace

ChatConversationModel::ChatConversationModel(QObject* parent)
    : QObject(parent) {
    m_persistencePool.setMaxThreadCount(1);
    m_persistencePool.setExpiryTimeout(-1);
}

ChatConversationModel::~ChatConversationModel() {
    m_persistencePool.waitForDone();
}

void ChatConversationModel::setDeferredPersistence(bool enabled) {
    m_deferredPersistence = enabled;
}

bool ChatConversationModel::initialize(const ProfileChatStoreOptions& options,
                                       QString* errorMessage) {
    if (errorMessage) errorMessage->clear();
    ++m_historyGeneration;
    m_initialized = false;
    m_persistenceAvailable = false;
    m_messages.clear();
    m_messageIndexes.clear();
    m_profileId.clear();
    m_lastReadMessageId.clear();
    const QString profileId = options.profileId.trimmed();
    const QUuid parsedProfileId(profileId);
    if (options.appDataRoot.trimmed().isEmpty() || parsedProfileId.isNull()
        || parsedProfileId.toString(QUuid::WithoutBraces).toLower() != profileId) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Chat history options are invalid.");
        }
        return false;
    }
    m_profileId = profileId;

    QString storeError;
    if (!m_store.open(options, &storeError)) {
        m_initialized = true;
        if (errorMessage) *errorMessage = storeError;
        emit historyPersistenceWarning(
            storeError.isEmpty()
                ? QStringLiteral("Chat history is unavailable; messages will remain in memory.")
                : storeError);
        return true;
    }
    m_persistenceAvailable = true;

    QString loadWarning;
    m_messages = m_store.load(&loadWarning);
    for (int i = 0; i < m_messages.size(); ++i) {
        m_messageIndexes.insert(m_messages.at(i).id, i);
    }
    const QString storedReadMarker = QSettings().value(readMarkerKey()).toString();
    if (m_messageIndexes.contains(storedReadMarker)) {
        m_lastReadMessageId = storedReadMarker;
    }
    m_initialized = true;
    if (!loadWarning.isEmpty()) {
        emit historyPersistenceWarning(loadWarning);
        if (errorMessage) *errorMessage = loadWarning;
    }
    return true;
}

QString ChatConversationModel::appendUserMessage(const QString& text,
                                                 const QDateTime& timestamp) {
    const QString normalized = text.trimmed();
    if (!m_initialized || normalized.isEmpty()) return {};

    ChatHistoryEntry entry;
    entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    entry.role = QStringLiteral("user");
    entry.content = normalized;
    entry.timestamp = timestamp.isValid() ? timestamp : QDateTime::currentDateTime();
    entry.status = ChatMessageStatus::Complete;
    const int index = m_messages.size();
    m_messages.append(entry);
    m_messageIndexes.insert(entry.id, index);
    emit messageInserted(index, entry.id);
    appendToStore(entry);
    return entry.id;
}

void ChatConversationModel::beginAssistantMessage(const QString& messageId,
                                                  const QString& replyToId,
                                                  const QDateTime& timestamp,
                                                  const QStringList& sourceMessageIds) {
    if (!m_initialized || messageId.trimmed().isEmpty()
        || m_messageIndexes.contains(messageId)) {
        if (m_initialized) warnUnknownMessage();
        return;
    }
    if (!replyToId.isEmpty()) {
        const int replyIndex = indexOf(replyToId);
        if (replyIndex < 0
            || m_messages.at(replyIndex).role != QStringLiteral("user")) {
            warnUnknownMessage();
            return;
        }
    }

    ChatHistoryEntry entry;
    entry.id = messageId;
    entry.role = QStringLiteral("assistant");
    entry.replyToId = replyToId;
    entry.sourceMessageIds = sourceMessageIds;
    entry.timestamp = timestamp.isValid() ? timestamp : QDateTime::currentDateTime();
    entry.status = ChatMessageStatus::Pending;
    const int index = m_messages.size();
    m_messages.append(entry);
    m_messageIndexes.insert(entry.id, index);
    emit messageInserted(index, entry.id);
}

QString ChatConversationModel::sourceInputForReply(const QString& assistantMessageId) const {
    const int index = indexOf(assistantMessageId);
    if (index < 0 || m_messages.at(index).role != QLatin1String("assistant")) return {};
    const auto& assistant = m_messages.at(index);
    const QStringList ids = assistant.sourceMessageIds.isEmpty()
        ? QStringList{assistant.replyToId} : assistant.sourceMessageIds;
    QStringList fragments;
    for (const auto& id : ids) {
        const int source = indexOf(id);
        if (source < 0 || m_messages.at(source).role != QLatin1String("user")) return {};
        fragments.append(m_messages.at(source).content);
    }
    return fragments.join('\n');
}

void ChatConversationModel::appendAssistantDelta(const QString& messageId,
                                                 const QString& delta) {
    if (delta.isEmpty()) return;
    const int index = indexOf(messageId);
    if (index < 0) {
        warnUnknownMessage();
        return;
    }
    ChatHistoryEntry& entry = m_messages[index];
    if (entry.role != QStringLiteral("assistant")
        || (entry.status != ChatMessageStatus::Pending
            && entry.status != ChatMessageStatus::Streaming)) {
        warnUnknownMessage();
        return;
    }
    entry.content.append(delta);
    entry.status = ChatMessageStatus::Streaming;
    emit messageChanged(index, entry.id);
}

void ChatConversationModel::setAssistantStage(const QString& messageId,
                                              ChatActivityStage stage) {
    const int index = indexOf(messageId);
    if (index < 0 || m_messages.at(index).role != QStringLiteral("assistant")
        || isTerminalChatMessageStatus(m_messages.at(index).status)) {
        warnUnknownMessage();
        return;
    }
    emit assistantStageChanged(messageId, stage);
}

void ChatConversationModel::finishAssistantMessage(const QString& messageId,
                                                   ChatMessageStatus status,
                                                   const QString& errorMessage) {
    const int index = indexOf(messageId);
    if (index < 0) {
        warnUnknownMessage();
        return;
    }
    ChatHistoryEntry& entry = m_messages[index];
    if (entry.role != QStringLiteral("assistant")
        || !canFinish(entry.status, status)) {
        warnUnknownMessage();
        return;
    }
    entry.status = status;
    entry.errorMessage = status == ChatMessageStatus::Complete
        ? QString()
        : sanitizedChatError(errorMessage);
    emit messageChanged(index, entry.id);
    appendToStore(entry);
}

void ChatConversationModel::markReadThrough(const QString& messageId) {
    if (!m_initialized || !m_messageIndexes.contains(messageId)) return;
    m_lastReadMessageId = messageId;
    QSettings settings;
    settings.setValue(readMarkerKey(), messageId);
    settings.sync();
}

QString ChatConversationModel::lastReadMessageId() const {
    return m_lastReadMessageId;
}

QList<ChatHistoryEntry> ChatConversationModel::messages() const {
    return m_messages;
}

QJsonObject ChatConversationModel::recentHistoryPage(int limit) const {
    QList<ChatHistoryEntry> visible, diagnostics;
    for (const auto& entry : m_messages) {
        if (isVisibleChatHistoryEntry(entry)) visible.append(entry);
        else diagnostics.append(entry);
    }
    const int first = std::max(0, int(visible.size()) - qBound(1, limit, 80));
    QJsonObject page = chatHistoryPage(visible.mid(first), first > 0, false);
    QJsonArray diagnosticMessages;
    for (const auto& entry : diagnostics.mid(std::max(qsizetype(0), diagnostics.size() - 20)))
        diagnosticMessages.append(chatHistoryEntryJson(entry));
    page["diagnosticMessages"] = diagnosticMessages;
    return page;
}

void ChatConversationModel::requestHistory(const QString& action,
                                           const QJsonObject& request,
                                           ChatHistoryCompletion completion) {
    const auto normalized = normalizeChatHistoryQuery(request, action == "search_chat_history");
    const auto store = m_store;
    const bool useStore = m_persistenceAvailable;
    // Only memory-only sessions need a copy of the working set. Persisted history
    // must be queried independently, including records outside the loaded window.
    const auto entries = useStore ? QList<ChatHistoryEntry>{} : m_messages;
    const quint64 generation = m_historyGeneration;
    // Destruction waits for this queue before QObject teardown. The queued callback
    // has this object as receiver, so Qt drops it if the model has gone away.
    m_persistencePool.start([this, store, useStore, entries, normalized, generation,
                            completion = std::move(completion)]() mutable {
        auto result = !normalized ? normalized
            : useStore ? store.queryHistory(normalized.value())
            : queryChatHistoryEntries(entries, normalized.value());
        QMetaObject::invokeMethod(this,
            [this, generation, completion = std::move(completion), result = std::move(result)]() mutable {
                if (generation == m_historyGeneration && completion) completion(std::move(result));
            }, Qt::QueuedConnection);
    });
}

int ChatConversationModel::indexOf(const QString& messageId) const {
    const auto found = m_messageIndexes.constFind(messageId);
    return found == m_messageIndexes.cend() ? -1 : found.value();
}

bool ChatConversationModel::appendToStore(const ChatHistoryEntry& entry) {
    if (!m_persistenceAvailable) return false;
    if (m_deferredPersistence) {
        ProfileChatHistoryStore store = m_store;
        QPointer<ChatConversationModel> guarded(this);
        m_persistencePool.start(
            [store = std::move(store), entry, guarded]() mutable {
                QString error;
                if (store.appendFinal(entry, &error)) return;
                const QString warning = error.isEmpty()
                    ? QStringLiteral("Unable to save chat history.")
                    : error;
                if (!guarded) return;
                QMetaObject::invokeMethod(
                    guarded.data(),
                    [guarded, warning]() {
                        if (guarded) {
                            emit guarded->historyPersistenceWarning(warning);
                        }
                    },
                    Qt::QueuedConnection);
            });
        return true;
    }
    QString error;
    if (m_store.appendFinal(entry, &error)) return true;
    emit historyPersistenceWarning(
        error.isEmpty() ? QStringLiteral("Unable to save chat history.") : error);
    return false;
}

void ChatConversationModel::warnUnknownMessage() {
    emit historyPersistenceWarning(
        QStringLiteral("A chat update was ignored because its message lifecycle was invalid."));
}

QString ChatConversationModel::readMarkerKey() const {
    return QStringLiteral("chat/%1/lastReadMessageId").arg(m_profileId);
}
