#ifndef DESKTOP_PET_CONVERSATION_TYPES_H
#define DESKTOP_PET_CONVERSATION_TYPES_H
#include "ai/context/context_spec.h"
#include "ai/domain/domain_result.h"
#include "ai_types.h"
#include <QDateTime>
#include <functional>
#include <optional>
#include <memory>

struct ToolDispatchToken;

enum class ConversationKind { LocalOwner, Direct, Group };
struct ReplyTarget {
    QString channel;
    QString connectionKey;
    QString conversationId;
    QString remoteConversationId;
    QString replyToMessageId;
    bool operator==(const ReplyTarget&) const = default;
    QJsonObject toJson() const;
    static ReplyTarget fromJson(const QJsonObject& value);
};
struct IncomingMessage {
    QString channel;
    QString connectionKey;
    QString remoteConversationId;
    QString remoteMessageId;
    QString sourceEventId;
    QString localMessageId;
    ExternalIdentity speakerIdentity;
    QString content;
    QString replyToRemoteId;
    QDateTime occurredAt;
    QDateTime receivedAt;
    bool originalTimeKnown = true;
    bool historicalImport = false;
    ConversationKind kind = ConversationKind::Direct;
    QString title;
    OriginKind originKind = OriginKind::External;
    QJsonArray images;
};
struct AcceptedMessage {
    QString messageId;
    QString conversationId;
    PersonResolution personResolution;
    bool duplicate = false;
    bool selfMessage = false;
    bool historicalImport = false;
    bool originalTimeKnown = true;
    QDateTime receivedAt;
    qint64 revision = 0;
    QString originId;
    ContextSpec context;
    ReplyTarget replyTarget;
};
struct ContextRequestV2 {
    ContextSpec context;
    QString requestId;
    quint64 generation = 0;
    QString reason;
    ReplyTarget replyTarget;
    QStringList sourceMessageIds;
    int historyLimit = 20;
};
struct PreparedContext {
    ContextSpec context;
    QString requestId;
    quint64 generation = 0;
    ReplyTarget replyTarget;
    QList<ChatMessage> messages;
    QStringList sourceMessageIds;
    QStringList originIds;
    QString subjectPersonId;
    QJsonArray currentInputMessages;
};
struct RoutedResponse {
    QString requestId;
    quint64 generation = 0;
    ContextSpec context;
    ReplyTarget replyTarget;
    QString content;
    QStringList sourceMessageIds;
};
enum class DeliveryOutcome { Completed, NotExecuted, Unknown };
struct DeliveryReceipt {
    QString requestId;
    quint64 generation = 0;
    ReplyTarget replyTarget;
    DeliveryOutcome outcome = DeliveryOutcome::Unknown;
    QString remoteMessageId;
    QString content;
    QDateTime occurredAt;
    QJsonObject evidence;
};
struct ReplyDispatch {
    RoutedResponse response;
    QString toolName;
    QJsonObject arguments;
    // Shared host authority; asynchronous preparation does not begin a send.
    std::shared_ptr<ToolDispatchToken> dispatchToken;
};
using ReplyCompletion = std::function<void(DeliveryReceipt)>;
using ReplyDispatcher = std::function<void(const ReplyDispatch&, ReplyCompletion)>;
struct HostConversationBinding {
    QString channel;
    QString connectionKey;
    QString remoteConversationId;
    ConversationKind kind = ConversationKind::Direct;
    ExternalIdentity selfIdentity;
    QString toolName;
    // Dispatcher owns the host policy context and runs the configured tool asynchronously.
    ReplyDispatcher dispatcher;
};
QJsonObject contextSpecToJson(const ContextSpec& context);
ContextSpec contextSpecFromJson(const QJsonObject& value);
QString conversationBindingKey(const QString& channel, const QString& connection,
                               const QString& remote);
QString conversationIdFor(const QString& profile, const QString& channel,
                          const QString& connection, const QString& remote);
#endif
