#include "observation_store.h"
#include "ai/event/runtime_unit_of_work.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <algorithm>
#include <mutex>
namespace {
std::mutex acceptanceMutex;
QHash<QString, qint64> failedSequences;
using V = Result<void, DomainError>;
QString json(const QJsonObject &o) {
  return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}
QString digest(const QString &s) {
  return QString::fromLatin1(
      QCryptographicHash::hash(s.toUtf8(), QCryptographicHash::Sha256).toHex());
}
DomainError bad(const QString &s) { return domainError("INVALID_INPUT", s); }
DomainError sqlError(const QSqlQuery &q) {
  return domainError("STORE_UNAVAILABLE", q.lastError().text());
}
V exec(QSqlDatabase db, const QString &sql, const QVariantList &args) {
  QSqlQuery q(db);
  q.prepare(sql);
  for (const auto &a : args)
    q.addBindValue(a);
  return q.exec() ? V::success() : V::failure(sqlError(q));
}
Result<QJsonObject, DomainError> parse(const QVariant &v) {
  QJsonParseError e;
  const auto d = QJsonDocument::fromJson(v.toByteArray(), &e);
  return e.error == QJsonParseError::NoError && d.isObject()
             ? Result<QJsonObject, DomainError>::success(d.object())
             : Result<QJsonObject, DomainError>::failure(domainError(
                   "STORE_UNAVAILABLE", "Invalid observation marker"));
}
Result<QJsonValue, DomainError> at(QJsonValue v, const QString &ptr) {
  if (ptr.isEmpty())
    return Result<QJsonValue, DomainError>::success(v);
  for (auto key : ptr.mid(1).split('/')) {
    key.replace("~1", "/");
    key.replace("~0", "~");
    if (v.isObject())
      v = v.toObject().value(key);
    else if (v.isArray()) {
      bool ok = false;
      int i = key.toInt(&ok);
      v = ok && i >= 0 && i < v.toArray().size()
              ? v.toArray()[i]
              : QJsonValue(QJsonValue::Undefined);
    } else
      v = QJsonValue(QJsonValue::Undefined);
    if (v.isUndefined())
      return Result<QJsonValue, DomainError>::failure(
          bad("Mapped result field is absent"));
  }
  return Result<QJsonValue, DomainError>::success(v);
}
bool text(const QJsonObject &o, const QString &key, int max = 1024) {
  return o[key].isString() && !o[key].toString().isEmpty() &&
         o[key].toString().size() <= max;
}
QString sourceKey(const ObservationBinding &b) {
  return "observation:" +
         digest(json({{"id", b.id},
                      {"pluginId", b.pluginId},
                      {"channel", b.channel},
                      {"connectionKey", b.connectionKey},
                      {"provider", b.provider},
                      {"namespace", b.nameSpace},
                      {"self", b.selfIdentity.remoteActorId}}));
}
} // namespace
Result<ObservationAcknowledgement, DomainError>
ObservationStore::ingest(const ObservationBinding &binding,
                         const ObservationEnvelope &envelope) {
  using R = Result<ObservationAcknowledgement, DomainError>;
  const auto o = envelope.payload;
  const QStringList allowed{
      "type",       "kind",         "eventId", "sequence", "cursor",
      "occurredAt", "conversation", "speaker", "message",  "historicalImport"};
  for (auto it = o.begin(); it != o.end(); ++it)
    if (!allowed.contains(it.key()))
      return R::failure(bad("Unknown observation field"));
  bool sequenceOk = false;
  const auto sequence = o["sequence"].toString().toLongLong(&sequenceOk);
  if (binding.id.isEmpty() || envelope.bindingId != binding.id ||
      o["type"] != "observation" || o["kind"] != "message" ||
      !text(o, "eventId") || !text(o, "cursor", 8192) ||
      !o["sequence"].isString() ||
      !QRegularExpression("^[1-9][0-9]*$")
           .match(o["sequence"].toString())
           .hasMatch() ||
      !sequenceOk || sequence <= 0 ||
      QString::number(sequence) != o["sequence"].toString() ||
      !o["conversation"].isObject() || !o["speaker"].isObject() ||
      !o["message"].isObject() ||
      QJsonDocument(o).toJson(QJsonDocument::Compact).size() > 65536 ||
      (o.contains("historicalImport") && !o["historicalImport"].isBool()))
    return R::failure(bad("Invalid observation envelope"));
  const auto conversation = o["conversation"].toObject(),
             speaker = o["speaker"].toObject(),
             message = o["message"].toObject();
  auto checkKeys = [](const QJsonObject &obj, const QStringList &keys) {
    for (auto it = obj.begin(); it != obj.end(); ++it)
      if (!keys.contains(it.key()))
        return false;
    return true;
  };
  if (!checkKeys(conversation, {"remoteConversationId", "kind", "title"}) ||
      !checkKeys(speaker,
                 {"provider", "namespace", "remoteActorId", "displayName"}) ||
      !checkKeys(message, {"remoteMessageId", "text", "replyToRemoteId"}) ||
      !text(conversation, "remoteConversationId") ||
      !text(message, "remoteMessageId") || !text(message, "text", 8000) ||
      message["text"].toString().trimmed().isEmpty() ||
      speaker["provider"] != binding.provider ||
      speaker["namespace"] != binding.nameSpace)
    return R::failure(bad("Invalid observation scope or message"));
  for (const auto &k : {"remoteActorId", "displayName"})
    if (speaker.contains(k) && !speaker[k].isString())
      return R::failure(bad("Invalid speaker field"));
  if ((message.contains("replyToRemoteId") &&
       !message["replyToRemoteId"].isString()) ||
      (conversation.contains("title") && !conversation["title"].isString()))
    return R::failure(bad("Invalid message reference"));
  std::optional<HostConversationBinding> host;
  for (const auto &c : binding.conversations)
    if (c.remoteConversationId ==
            conversation["remoteConversationId"].toString() &&
        conversation["kind"] ==
            (c.kind == ConversationKind::Group ? "group" : "direct"))
      host = c;
  if (!host)
    return R::failure(
        domainError("PERMISSION_DENIED",
                    "Observation conversation is not host-authorized"));
  QDateTime occurred;
  if (o.contains("occurredAt")) {
    if (!o["occurredAt"].isString())
      return R::failure(bad("Invalid original timestamp"));
    occurred =
        QDateTime::fromString(o["occurredAt"].toString(), Qt::ISODateWithMs);
    if (!occurred.isValid())
      return R::failure(bad("Invalid original timestamp"));
  }
  IncomingMessage incoming;
  incoming.channel = binding.channel;
  incoming.connectionKey = binding.connectionKey;
  incoming.remoteConversationId = host->remoteConversationId;
  incoming.kind = host->kind;
  incoming.title = conversation["title"].toString();
  incoming.remoteMessageId = message["remoteMessageId"].toString();
  incoming.sourceEventId = o["eventId"].toString();
  incoming.content = message["text"].toString();
  incoming.replyToRemoteId = message["replyToRemoteId"].toString();
  incoming.speakerIdentity = {binding.provider, binding.nameSpace,
                              speaker["remoteActorId"].toString(),
                              speaker["displayName"].toString()};
  incoming.occurredAt = occurred;
  incoming.originalTimeKnown = occurred.isValid();
  incoming.receivedAt = QDateTime::currentDateTimeUtc();
  incoming.historicalImport = o["historicalImport"].toBool();
  // Display labels and host receipt time are not journal identity. Missing
  // remote time from one transport must not turn a replay into a conflicting
  // event.
  QJsonObject canonical{{"remoteConversationId", incoming.remoteConversationId},
                        {"kind", conversation["kind"]},
                        {"remoteMessageId", incoming.remoteMessageId},
                        {"text", incoming.content},
                        {"speaker", speaker["remoteActorId"].toString()},
                        {"replyToRemoteId", incoming.replyToRemoteId},
                        {"historicalImport", incoming.historicalImport}};
  ObservationAcknowledgement ack;
  ack.eventId = o["eventId"].toString();
  ConversationStore store(m_profile, m_path);
  auto registered = store.registerBinding(*host);
  if (!registered)
    return R::failure(registered.error());
  const auto source = sourceKey(binding);
  const auto gate = m_profile + "/" + m_path + "/" + source;
  std::lock_guard serialized(acceptanceMutex);
  if (failedSequences.contains(gate) && sequence > failedSequences[gate])
    return R::failure(
        domainError("STORE_UNAVAILABLE",
                    "Earlier source fact must commit before this watermark"));
  auto result = store.acceptMessage(
      incoming, [&](RuntimeUnitOfWork &unit, AcceptedMessage &accepted) -> V {
        auto db = QSqlDatabase::database(unit.connectionName());
        const QJsonObject marker{
            {"eventId", ack.eventId},
            {"sequence", o["sequence"]},
            {"cursor", o["cursor"]},
            {"content", canonical},
            {"occurredAt", occurred.isValid()
                               ? occurred.toUTC().toString(Qt::ISODateWithMs)
                               : QString()},
            {"messageId", accepted.messageId},
            {"originId", accepted.originId},
            {"bindingId", binding.id}};
        const QStringList markerIds{
            source + ":event:" + digest(ack.eventId),
            source + ":sequence:" + o["sequence"].toString()};
        for (const auto &id : markerIds) {
          QSqlQuery q(db);
          q.prepare("SELECT cursor_json FROM agent_runtime_migrations WHERE "
                    "profile_id=? AND migration_id=?");
          q.addBindValue(m_profile);
          q.addBindValue(id);
          if (!q.exec())
            return V::failure(sqlError(q));
          if (q.next()) {
            auto previous = parse(q.value(0));
            if (!previous)
              return V::failure(previous.error());
            const auto p = previous.value();
            if (p["eventId"] != marker["eventId"] ||
                p["sequence"] != marker["sequence"] ||
                p["cursor"] != marker["cursor"] ||
                p["content"] != marker["content"] ||
                (!p["occurredAt"].toString().isEmpty() && occurred.isValid() &&
                 p["occurredAt"] != marker["occurredAt"]))
              return V::failure(
                  bad("Conflicting observation journal event or sequence"));
          } else {
            auto saved = exec(db,
                              "INSERT INTO "
                              "agent_runtime_migrations(profile_id,migration_"
                              "id,cursor_json,state) VALUES(?,?,?,'completed')",
                              {m_profile, id, json(marker)});
            if (!saved)
              return saved;
          }
        }
        QSqlQuery q(db);
        q.prepare("SELECT revision,control_json FROM agent_runtime_state WHERE "
                  "profile_id=?");
        q.addBindValue(m_profile);
        if (!q.exec())
          return V::failure(sqlError(q));
        bool exists = q.next();
        qint64 revision = exists ? q.value(0).toLongLong() : 0;
        QJsonObject control;
        if (exists) {
          auto p = parse(q.value(1));
          if (!p)
            return V::failure(p.error());
          control = p.value();
        }
        if (control.contains("sourceCursors") &&
            !control["sourceCursors"].isObject())
          return V::failure(domainError("STORE_UNAVAILABLE",
                                        "Invalid persisted source cursors"));
        auto cursors = control["sourceCursors"].toObject();
        if (cursors.contains(source) && !cursors[source].isObject())
          return V::failure(domainError("STORE_UNAVAILABLE",
                                        "Invalid persisted source node"));
        const auto old = cursors.value(source).toObject();
        bool oldOk = false;
        qint64 oldSequence = old["sequence"].toString().toLongLong(&oldOk);
        if (cursors.contains(source) &&
            (!oldOk || oldSequence < 1 ||
             QString::number(oldSequence) != old["sequence"].toString() ||
             !old["cursor"].isString() || old["cursor"].toString().isEmpty()))
          return V::failure(domainError("STORE_UNAVAILABLE",
                                        "Invalid persisted source sequence"));
        if (envelope.transport == ObservationTransport::Source &&
            sequence > oldSequence)
          cursors[source] =
              QJsonObject{{"sequence", o["sequence"]},
                          {"cursor", o["cursor"]},
                          {"generation", QString::number(envelope.generation)}};
        control["sourceCursors"] = cursors;
        ack.cursor =
            cursors.value(source).toObject().value("cursor").toString();
        auto updated = exists ? exec(db,
                                     "UPDATE agent_runtime_state SET "
                                     "control_json=?,revision=revision+1 WHERE "
                                     "profile_id=? AND revision=?",
                                     {json(control), m_profile, revision})
                              : exec(db,
                                     "INSERT INTO "
                                     "agent_runtime_state(profile_id,control_"
                                     "json) VALUES(?,?)",
                                     {m_profile, json(control)});
        if (!updated)
          return updated;
        QSqlQuery changed(db);
        if (!changed.exec("SELECT changes()") || !changed.next() ||
            changed.value(0).toInt() != 1)
          return V::failure(
              domainError("STORE_UNAVAILABLE",
                          "Source cursor compare-and-swap did not commit"));
        if (!accepted.duplicate && !accepted.selfMessage &&
            !accepted.historicalImport) {
          const QJsonObject cue{{"conversationId", accepted.conversationId},
                                {"eventId", accepted.originId},
                                {"messageId", accepted.messageId},
                                {"bindingId", binding.id}};
          auto saved = exec(
              db,
              "INSERT INTO "
              "agent_runtime_migrations(profile_id,migration_id,cursor_json,"
              "state) VALUES(?,?,?,'running')",
              {m_profile, "observation:cue:" + accepted.originId, json(cue)});
          if (!saved)
            return saved;
        }
        return V::success();
      });
  if (!result) {
    auto error = result.error();
    if (error.code == "EVENT_OUTBOX_UNAVAILABLE")
      error.code = "STORE_UNAVAILABLE";
    if (error.code == "STORE_UNAVAILABLE")
      failedSequences[gate] =
          qMin(sequence, failedSequences.value(gate, sequence));
    return R::failure(error);
  }
  if (failedSequences.value(gate) == sequence)
    failedSequences.remove(gate);
  ack.message = result.value();
  ack.committed = true;
  ack.duplicate = result.value().duplicate;
  return R::success(ack);
}
Result<QString, DomainError>
ObservationStore::cursor(const ObservationBinding &binding) const {
  using R = Result<QString, DomainError>;
  auto u = SqliteRuntimeUnitOfWork::begin(m_path);
  if (!u)
    return R::failure(u.error());
  QSqlQuery q(QSqlDatabase::database(u.value()->connectionName()));
  q.prepare("SELECT control_json FROM agent_runtime_state WHERE profile_id=?");
  q.addBindValue(m_profile);
  if (!q.exec())
    return R::failure(sqlError(q));
  if (!q.next())
    return R::success({});
  auto parsed = parse(q.value(0));
  if (!parsed)
    return R::failure(parsed.error());
  const auto control = parsed.value();
  if (!control.contains("sourceCursors"))
    return R::success({});
  if (!control["sourceCursors"].isObject())
    return R::failure(
        domainError("STORE_UNAVAILABLE", "Invalid persisted source cursors"));
  const auto cursors = control["sourceCursors"].toObject();
  if (!cursors.contains(sourceKey(binding)))
    return R::success({});
  const auto value = cursors[sourceKey(binding)];
  const auto entry = value.toObject();
  bool ok = false;
  const auto seq = entry["sequence"].toString().toLongLong(&ok);
  if (!value.isObject() || !ok || seq < 1 ||
      QString::number(seq) != entry["sequence"].toString() ||
      !entry["cursor"].isString() || entry["cursor"].toString().isEmpty())
    return R::failure(
        domainError("STORE_UNAVAILABLE", "Invalid persisted source cursor"));
  return R::success(entry["cursor"].toString());
}
Result<std::optional<PendingObservationCue>, DomainError>
ObservationStore::pendingCue(QSqlDatabase db, const QString &profile) {
  using R = Result<std::optional<PendingObservationCue>, DomainError>;
  QSqlQuery q(db);
  q.prepare("SELECT m.cursor_json FROM agent_runtime_migrations m JOIN "
            "event_log e ON e.profile_id=m.profile_id AND "
            "e.event_id=json_extract(m.cursor_json,'$.eventId') WHERE "
            "m.profile_id=? AND m.migration_id LIKE 'observation:cue:%' AND "
            "m.state='running' ORDER BY m.rowid LIMIT 256");
  q.addBindValue(profile);
  if (!q.exec())
    return R::failure(sqlError(q));
  std::optional<PendingObservationCue> cue;
  while (q.next()) {
    auto p = parse(q.value(0));
    if (!p)
      return R::failure(p.error());
    const auto c = p.value();
    if (!cue)
      cue = PendingObservationCue{c["conversationId"].toString(), {}, {}};
    if (c["conversationId"] == cue->conversationId &&
        cue->eventIds.size() < 8) {
      cue->eventIds.append(c["eventId"].toString());
      cue->watermark[cue->conversationId] = c["messageId"];
    }
  }
  return R::success(cue);
}
V ObservationStore::consumeCues(QSqlDatabase db, const QString &profile,
                                const QStringList &ids) {
  for (const auto &id : ids) {
    auto r = exec(db,
                  "UPDATE agent_runtime_migrations SET state='completed' WHERE "
                  "profile_id=? AND migration_id=?",
                  {profile, "observation:cue:" + id});
    if (!r)
      return r;
  }
  return V::success();
}
Result<QList<ObservationEnvelope>, DomainError>
ObservationStore::normalize(const ObservationAdapter &a,
                            const ObservationBinding &b,
                            const CapabilityOutcome &outcome) {
  using R = Result<QList<ObservationEnvelope>, DomainError>;
  if (outcome.stage != "completed")
    return R::success({});
  auto items = at(outcome.data, a.itemsPointer);
  if (!items || !items.value().isArray() ||
      items.value().toArray().size() > 256)
    return R::failure(bad("Observation result requires an array of at most 256 "
                          "items; paginate the adapter"));
  QList<ObservationEnvelope> result;
  for (const auto &item : items.value().toArray()) {
    QJsonObject fields;
    for (auto it = a.fields.begin(); it != a.fields.end(); ++it) {
      auto v = at(item, it.value().toString());
      if (v)
        fields[it.key()] = v.value();
    }
    QJsonObject conversation{
        {"remoteConversationId", fields["remoteConversationId"]}};
    for (const auto &c : b.conversations)
      if (c.remoteConversationId == fields["remoteConversationId"].toString())
        conversation["kind"] =
            c.kind == ConversationKind::Group ? "group" : "direct";
    QJsonObject speaker{{"provider", b.provider}, {"namespace", b.nameSpace}};
    if (fields.contains("remoteActorId"))
      speaker["remoteActorId"] = fields["remoteActorId"];
    if (fields.contains("speakerDisplayName"))
      speaker["displayName"] = fields["speakerDisplayName"];
    QJsonObject msg{{"remoteMessageId", fields["remoteMessageId"]},
                    {"text", fields["text"]}};
    if (fields.contains("replyToRemoteId"))
      msg["replyToRemoteId"] = fields["replyToRemoteId"];
    QJsonObject payload{
        {"type", "observation"},        {"kind", "message"},
        {"eventId", fields["eventId"]}, {"sequence", fields["sequence"]},
        {"cursor", fields["cursor"]},   {"conversation", conversation},
        {"speaker", speaker},           {"message", msg}};
    for (const auto &k : {"occurredAt", "historicalImport"})
      if (fields.contains(k))
        payload[k] = fields[k];
    result.append(
        {b.id, payload, outcome.catalogRevision, ObservationTransport::Query});
  }
  std::stable_sort(
      result.begin(), result.end(),
      [](const ObservationEnvelope &x, const ObservationEnvelope &y) {
        return x.payload["sequence"].toString().toLongLong() <
               y.payload["sequence"].toString().toLongLong();
      });
  return R::success(result);
}
