#include "observation_types.h"
#include <QDir>
#include <QRegularExpression>
#include <QSet>
namespace {
bool text(const QJsonObject &o, const QString &key) {
  return o[key].isString() && !o[key].toString().trimmed().isEmpty() &&
         o[key].toString().size() <= 1024;
}
bool keys(const QJsonObject &o, const QStringList &allowed) {
  for (auto it = o.begin(); it != o.end(); ++it)
    if (!allowed.contains(it.key()))
      return false;
  return true;
}
bool pointer(const QJsonValue &v) {
  return v.isString() && QRegularExpression("^(?:/(?:[^~/]|~[01])*)*$")
                             .match(v.toString())
                             .hasMatch();
}
} // namespace
Result<ObservationConfiguration, DomainError>
ObservationConfiguration::parse(const QJsonObject &config) {
  using R = Result<ObservationConfiguration, DomainError>;
  auto bad = [] {
    return R::failure(domainError(
        "INVALID_INPUT", "Invalid or ambiguous observation host binding"));
  };
  ObservationConfiguration result;
  for (const auto &name : {"observationBindings", "observationAdapters"})
    if (config.contains(name) &&
        (!config[name].isArray() || config[name].toArray().size() > 256))
      return bad();
  QSet<QString> sourceKeys;
  for (const auto &value : config["observationBindings"].toArray()) {
    if (!value.isObject())
      return bad();
    const auto o = value.toObject();
    if (!keys(o, {"id", "pluginId", "channel", "connectionKey", "provider",
                  "namespace", "selfIdentity", "conversations", "source"}))
      return bad();
    for (const auto &k : {"id", "pluginId", "channel", "connectionKey",
                          "provider", "namespace"})
      if (!text(o, k))
        return bad();
    if (result.bindings.contains(o["id"].toString()) ||
        !o["selfIdentity"].isObject() || !o["conversations"].isArray() ||
        o["conversations"].toArray().size() > 256)
      return bad();
    ObservationBinding b;
    b.id = o["id"].toString();
    b.pluginId = o["pluginId"].toString();
    b.channel = o["channel"].toString();
    b.connectionKey = o["connectionKey"].toString();
    b.provider = o["provider"].toString();
    b.nameSpace = o["namespace"].toString();
    const auto self = o["selfIdentity"].toObject();
    if (!keys(self,
              {"provider", "namespace", "remoteActorId", "displayName"}) ||
        self["provider"] != b.provider || self["namespace"] != b.nameSpace ||
        !text(self, "remoteActorId"))
      return bad();
    b.selfIdentity = {b.provider, b.nameSpace, self["remoteActorId"].toString(),
                      self["displayName"].toString()};
    QSet<QString> rooms;
    for (const auto &c : o["conversations"].toArray()) {
      if (!c.isObject())
        return bad();
      const auto room = c.toObject();
      if (!keys(room, {"remoteConversationId", "kind", "title"}) ||
          !text(room, "remoteConversationId") ||
          (room["kind"] != "direct" && room["kind"] != "group") ||
          (room.contains("title") && !room["title"].isString()) ||
          rooms.contains(room["remoteConversationId"].toString()))
        return bad();
      HostConversationBinding h;
      h.channel = b.channel;
      h.connectionKey = b.connectionKey;
      h.remoteConversationId = room["remoteConversationId"].toString();
      h.kind = room["kind"] == "group" ? ConversationKind::Group
                                       : ConversationKind::Direct;
      h.selfIdentity = b.selfIdentity;
      b.conversations.append(h);
      rooms.insert(h.remoteConversationId);
    }
    if (o.contains("source")) {
      if (!o["source"].isObject())
        return bad();
      b.source = o["source"].toObject();
      if (!keys(b.source,
                {"sourceId", "configurationKey", "executor", "enabled"}) ||
          !text(b.source, "sourceId") || !text(b.source, "configurationKey") ||
          !text(b.source, "executor") ||
          !QDir::isAbsolutePath(b.source["executor"].toString()) ||
          (b.source.contains("enabled") && !b.source["enabled"].isBool()))
        return bad();
      const auto key = b.pluginId + "/" + b.source["sourceId"].toString() +
                       "/" + b.connectionKey;
      if (sourceKeys.contains(key))
        return bad();
      sourceKeys.insert(key);
    }
    result.bindings.insert(b.id, b);
  }
  QSet<QString> refs;
  const QStringList required{"eventId",         "sequence",
                             "cursor",          "remoteConversationId",
                             "remoteMessageId", "text"};
  const auto allowed =
      required + QStringList{"remoteActorId", "speakerDisplayName",
                             "occurredAt", "replyToRemoteId",
                             "historicalImport"};
  for (const auto &value : config["observationAdapters"].toArray()) {
    if (!value.isObject())
      return bad();
    const auto o = value.toObject();
    if (!keys(o, {"ref", "observationBindingId", "itemsPointer", "fields"}) ||
        !o["ref"].isObject() || !text(o, "observationBindingId") ||
        !pointer(o["itemsPointer"]) || !o["fields"].isObject())
      return bad();
    const auto refObject = o["ref"].toObject();
    if (!keys(refObject,
              {"pluginId", "entryId", "entryVersion", "operationId"}))
      return bad();
    for (const auto &k : {"pluginId", "entryId", "entryVersion", "operationId"})
      if (!text(refObject, k))
        return bad();
    ObservationAdapter a;
    a.ref = CapabilityRef::fromJson(refObject);
    a.bindingId = o["observationBindingId"].toString();
    a.itemsPointer = o["itemsPointer"].toString();
    a.fields = o["fields"].toObject();
    if (!result.bindings.contains(a.bindingId) ||
        result.bindings[a.bindingId].pluginId != a.ref.pluginId ||
        !keys(a.fields, allowed))
      return bad();
    for (const auto &k : required)
      if (!a.fields.contains(k))
        return bad();
    for (auto it = a.fields.begin(); it != a.fields.end(); ++it)
      if (!pointer(it.value()))
        return bad();
    // One exact result cannot be assigned to two accounts without an account
    // discriminator.
    if (refs.contains(a.ref.key()))
      return bad();
    refs.insert(a.ref.key());
    result.adapters.append(a);
  }
  return R::success(result);
}
