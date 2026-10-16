#include "conversation_types.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>
QJsonObject ReplyTarget::toJson() const {
    return {{"channel", channel}, {"connectionKey", connectionKey},
            {"conversationId", conversationId}, {"remoteConversationId", remoteConversationId},
            {"replyToMessageId", replyToMessageId}};
}
ReplyTarget ReplyTarget::fromJson(const QJsonObject& j) {
    return {j["channel"].toString(), j["connectionKey"].toString(),
            j["conversationId"].toString(), j["remoteConversationId"].toString(),
            j["replyToMessageId"].toString()};
}
QJsonObject contextSpecToJson(const ContextSpec& c) {
    return {{"profileId", c.profileId}, {"channel", c.channel},
            {"conversationId", c.conversationId}, {"activityId", c.activityId},
            {"participantPersonIds", QJsonArray::fromStringList(c.participantPersonIds)},
            {"originKind", originKindToString(c.originKind)},
            {"conversationRevision", c.conversationRevision}, {"identityRevision", c.identityRevision},
            {"audienceProjectionRef", c.audienceProjectionRef}, {"allowOwnerMemory", c.allowOwnerMemory},
            {"allowInternalMemory", c.allowInternalMemory},
            {"authorizedConversationIds", QJsonArray::fromStringList(c.authorizedConversationIds)}};
}
ContextSpec contextSpecFromJson(const QJsonObject& j) {
    ContextSpec c;
    c.profileId = j["profileId"].toString(); c.channel = j["channel"].toString();
    c.conversationId = j["conversationId"].toString(); c.activityId = j["activityId"].toString();
    c.originKind = originKindFromString(j["originKind"].toString()).value_or(OriginKind::Internal);
    c.conversationRevision = j["conversationRevision"].toInteger();
    c.identityRevision = j["identityRevision"].toInteger();
    c.audienceProjectionRef = j["audienceProjectionRef"].toString();
    c.allowOwnerMemory = j["allowOwnerMemory"].toBool();
    c.allowInternalMemory = j["allowInternalMemory"].toBool();
    for (const auto& id : j["participantPersonIds"].toArray()) c.participantPersonIds.append(id.toString());
    for (const auto& id : j["authorizedConversationIds"].toArray()) c.authorizedConversationIds.append(id.toString());
    return c;
}
QString conversationBindingKey(const QString& channel, const QString& connection, const QString& remote) {
    return QString::fromUtf8(QJsonDocument(QJsonArray{channel, connection, remote}).toJson(QJsonDocument::Compact));
}
QString conversationIdFor(const QString& profile, const QString& channel,
                          const QString& connection, const QString& remote) {
    if (channel == "desktop" && connection == "local_owner" && remote == "local_owner") return "local_owner";
    return QUuid::createUuidV5(QUuid("312f2a64-cdc2-4fb7-a004-1f398bf10dca"),
        profile.toUtf8() + conversationBindingKey(channel, connection, remote).toUtf8()).toString(QUuid::WithoutBraces);
}
