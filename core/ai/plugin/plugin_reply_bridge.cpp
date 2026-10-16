#include "plugin_reply_bridge.h"
#include "ai/chat/conversation_store.h"
#include "ai/event/runtime_unit_of_work.h"
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
namespace {
template <class T>
Result<T, DomainError> fail(const QString &code, const QString &msg) {
  return Result<T, DomainError>::failure(domainError(code, msg));
}
} // namespace
Result<std::optional<PreparedCapabilityReply>, DomainError>
prepareCapabilityReply(const CapabilityCall &call, const QJsonObject &config,
                       const QString &path) {
  using O = std::optional<PreparedCapabilityReply>;
  using R = Result<O, DomainError>;
  QList<QJsonObject> matches;
  for (const auto &value : config["replyBindings"].toArray())
    if (CapabilityRef::fromJson(value.toObject()["ref"].toObject()) == call.ref)
      matches.append(value.toObject());
  if (matches.isEmpty())
    return R::success({});
  if (path.isEmpty())
    return fail<O>("DEPENDENCY_UNAVAILABLE",
                   "Reply archive database is unavailable");
  if (call.context.originKind != OriginKind::External ||
      call.context.conversationId.isEmpty())
    return fail<O>(
        "PERMISSION_DENIED",
        "External sending requires an external conversation context");
  std::optional<QJsonObject> match;
  for (const auto &value : matches) {
    const auto id =
        conversationIdFor(call.context.profileId, value["channel"].toString(),
                          value["connectionKey"].toString(),
                          value["remoteConversationId"].toString());
    if (id == call.context.conversationId) {
      if (match)
        return fail<O>("INVALID_INPUT", "Ambiguous reply binding");
      match = value;
    }
  }
  if (!match)
    return fail<O>("PERMISSION_DENIED",
                   "Send is outside the bound conversation");
  const auto value = *match;
  PreparedCapabilityReply prepared;
  prepared.binding.ref = call.ref;
  prepared.binding.grantRef = value["grantRef"].toString();
  prepared.binding.argumentBindings = value["argumentBindings"].toObject();
  prepared.binding.remoteMessageIdPointer =
      value["remoteMessageIdPointer"].toString("/remote_message_id");
  prepared.binding.contentPointer =
      value["contentPointer"].toString("/content");
  prepared.binding.occurredAtPointer =
      value["occurredAtPointer"].toString("/occurred_at");
  if (!prepared.binding.grantRef.isEmpty() &&
      prepared.binding.grantRef != call.grantRef)
    return fail<O>("PERMISSION_DENIED",
                   "Reply grant does not match the dispatched grant");
  QString text, replyRemote;
  bool targetSeen = false, textSeen = false, replySeen = false;
  for (auto it = prepared.binding.argumentBindings.begin();
       it != prepared.binding.argumentBindings.end(); ++it) {
    auto actual = CapabilityExecutor::pointer(call.arguments, it.key());
    if (!actual || !actual.value().isString())
      return fail<O>("INVALID_INPUT", "Invalid mapped send argument");
    if (it.value() == "remoteConversationId") {
      if (actual.value() != value["remoteConversationId"])
        return fail<O>("PERMISSION_DENIED",
                       "Send argument targets another conversation");
      targetSeen = true;
    } else if (it.value() == "content") {
      text = actual.value().toString();
      textSeen = true;
    } else if (it.value() == "replyToRemoteId") {
      replyRemote = actual.value().toString();
      replySeen = true;
    } else
      return fail<O>("INVALID_INPUT", "Unknown reply argument mapping");
  }
  if (!targetSeen || !textSeen || text.trimmed().isEmpty() ||
      text.size() > 8000)
    return fail<O>("INVALID_INPUT",
                   "Reply mapping requires a target and bounded text");
  ConversationStore store(call.context.profileId, path);
  ReplyTarget target{value["channel"].toString(),
                     value["connectionKey"].toString(),
                     call.context.conversationId,
                     value["remoteConversationId"].toString(),
                     {}};
  QStringList sources;
  auto history = store.loadHistory(call.context.conversationId, 64);
  if (!history)
    return R::failure(history.error());
  for (const auto &row : history.value())
    if (row.role == "user") {
      sources.append(row.id);
      while (sources.size() > 8)
        sources.removeFirst();
    }
  if (!replyRemote.isEmpty()) {
    auto u = SqliteRuntimeUnitOfWork::begin(path);
    if (!u)
      return R::failure(u.error());
    QSqlQuery q(QSqlDatabase::database(u.value()->connectionName()));
    q.prepare("SELECT id FROM agent_messages WHERE profile_id=? AND "
              "conversation_id=? AND remote_id=?");
    q.addBindValue(call.context.profileId);
    q.addBindValue(call.context.conversationId);
    q.addBindValue(replyRemote);
    if (!q.exec())
      return fail<O>("STORE_UNAVAILABLE", q.lastError().text());
    if (!q.next())
      return fail<O>("UNKNOWN_REFERENCE",
                     "Reply refers to an unobserved message");
    target.replyToMessageId = q.value(0).toString();
  }
  if (call.preparedResponse) {
    prepared.response = *call.preparedResponse;
    if (!replySeen)
      target.replyToMessageId = prepared.response.replyTarget.replyToMessageId;
    if (prepared.response.context.cacheKey() != call.context.cacheKey() ||
        prepared.response.content != text ||
        !(prepared.response.replyTarget == target))
      return fail<O>("INVALID_INPUT",
                     "Prepared dispatch does not match actual send");
    auto u = SqliteRuntimeUnitOfWork::begin(path);
    if (!u)
      return R::failure(u.error());
    QSqlQuery q(QSqlDatabase::database(u.value()->connectionName()));
    q.prepare("SELECT context_json,target_json,content,status FROM "
              "agent_response_dispatches WHERE profile_id=? AND request_id=? "
              "AND generation=?");
    q.addBindValue(call.context.profileId);
    q.addBindValue(prepared.response.requestId);
    q.addBindValue(prepared.response.generation);
    if (!q.exec())
      return fail<O>("STORE_UNAVAILABLE", q.lastError().text());
    if (!q.next() || q.value(2).toString() != text ||
        q.value(3).toString() != "dispatching" ||
        QJsonDocument::fromJson(q.value(0).toByteArray()).object() !=
            contextSpecToJson(call.context) ||
        QJsonDocument::fromJson(q.value(1).toByteArray()).object() !=
            target.toJson())
      return fail<O>("STALE_CONTEXT",
                     "Prepared reply dispatch is no longer available");
    return R::success(prepared);
  }
  auto valid = store.validateContext(call.context);
  if (!valid)
    return R::failure(valid.error());
  ContextRequestV2 request;
  request.context = call.context;
  request.requestId = call.callId;
  request.generation = 0;
  request.replyTarget = target;
  request.sourceMessageIds = sources;
  auto context = store.prepareContext(request);
  if (!context)
    return R::failure(context.error());
  prepared.response = {call.callId, 0,    context.value().context,
                       target,      text, sources};
  auto reserved = store.publishResponse(prepared.response);
  if (!reserved)
    return R::failure(reserved.error());
  return R::success(prepared);
}
DeliveryReceipt
capabilityDeliveryReceipt(const CapabilityOutcome &outcome,
                          const PreparedCapabilityReply &prepared) {
  DeliveryReceipt receipt;
  receipt.requestId = prepared.response.requestId;
  receipt.generation = prepared.response.generation;
  receipt.replyTarget = prepared.response.replyTarget;
  receipt.evidence = outcome.toJson();
  const auto id = CapabilityExecutor::pointer(
                 outcome.data, prepared.binding.remoteMessageIdPointer),
             text = CapabilityExecutor::pointer(
                 outcome.data, prepared.binding.contentPointer),
             time = CapabilityExecutor::pointer(
                 outcome.data, prepared.binding.occurredAtPointer);
  const auto occurred =
      time ? QDateTime::fromString(time.value().toString(), Qt::ISODateWithMs)
           : QDateTime{};
  if (outcome.stage == "completed" && id && id.value().isString() &&
      !id.value().toString().isEmpty() && text && text.value().isString() &&
      !text.value().toString().trimmed().isEmpty() &&
      text.value().toString().size() <= 8000 && occurred.isValid()) {
    receipt.outcome = DeliveryOutcome::Completed;
    receipt.remoteMessageId = id.value().toString();
    receipt.content = text.value().toString();
    receipt.occurredAt = occurred;
  } else
    receipt.outcome = outcome.executed ? DeliveryOutcome::Unknown
                                       : DeliveryOutcome::NotExecuted;
  return receipt;
}
