#include "conversation_store.h"
#include "ai/event/sqlite_event_repository.h"
#include "ai/event/event_schema_registry.h"
#include "ai/event/event_outbox.h"
#include "ai/event/runtime_unit_of_work.h"
#include "ai/identity/person_directory.h"
#include "ai/identity/sqlite_identity_repository.h"
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QJsonDocument>
#include <QJsonArray>
#include <QSet>
#include <QUuid>
#include <QCryptographicHash>
#include <limits>

namespace {
using VoidResult = Result<void, DomainError>;
DomainError storeError(const QString& what) { return domainError("STORE_UNAVAILABLE", what); }
DomainError invalid(const QString& what) { return domainError("INVALID_INPUT", what); }
QString json(const QJsonObject& value) { return QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact)); }
QString json(const QStringList& value) { return QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(value)).toJson(QJsonDocument::Compact)); }
QString stableId(const QString& kind, const QString& profile, const QString& seed) {
    return QUuid::createUuidV5(QUuid("0b6c8399-23d4-4eaa-88c6-0912d1f2907d"),
        QJsonDocument(QJsonArray{kind, profile, seed}).toJson(QJsonDocument::Compact)).toString(QUuid::WithoutBraces);
}
QString kindName(ConversationKind kind) {
    return kind == ConversationKind::LocalOwner ? "local_owner" : kind == ConversationKind::Group ? "group" : "direct";
}
bool sameIdentity(const ExternalIdentity& a, const ExternalIdentity& b) {
    return !a.remoteActorId.isEmpty() && !b.remoteActorId.isEmpty()
        && a.provider == b.provider && a.nameSpace == b.nameSpace && a.remoteActorId == b.remoteActorId;
}
QString nonnull(const QString& value) { return value.isNull() ? QStringLiteral("") : value; }
VoidResult run(QSqlDatabase& db, const QString& sql, const QVariantList& values = {}) {
    QSqlQuery q(db); q.prepare(sql);
    for (const auto& value : values) q.addBindValue(value);
    if (!q.exec()) return VoidResult::failure(storeError(q.lastError().text()));
    return VoidResult::success();
}
Result<std::unique_ptr<RuntimeUnitOfWork>, DomainError> beginConversationWrite(const QString& path) {
    using R=Result<std::unique_ptr<RuntimeUnitOfWork>, DomainError>;
    auto transaction=SqliteRuntimeUnitOfWork::begin(path);
    if(!transaction) return transaction;
    auto db=QSqlDatabase::database(transaction.value()->connectionName());
    const auto reserved=run(db,"UPDATE agent_conversations SET revision=revision WHERE 0");
    if(!reserved) return R::failure(reserved.error());
    return transaction;
}
Result<QJsonObject, DomainError> parsedObject(const QVariant& v) {
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(v.toByteArray(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return Result<QJsonObject, DomainError>::failure(storeError("Invalid persisted conversation payload"));
    return Result<QJsonObject, DomainError>::success(doc.object());
}
VoidResult ensureConversation(QSqlDatabase& db, const QString& profile, const QString& id,
                               const QString& channel, const QString& connection, const QString& remote,
                               ConversationKind kind, const QString& title = {}) {
    return run(db, "INSERT INTO agent_conversations(profile_id,id,channel,connection_key,remote_id,kind,title) "
                   "VALUES(?,?,?,?,?,?,?) ON CONFLICT(profile_id,id) DO NOTHING",
               {profile, id, channel, connection, remote, kindName(kind), nonnull(title)});
}
VoidResult incrementRevision(QSqlDatabase& db, const QString& profile, const QString& id) {
    return run(db, "UPDATE agent_conversations SET revision=revision+1,updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') "
                   "WHERE profile_id=? AND id=?", {profile,id});
}
Result<ContextSpec, DomainError> readContext(QSqlDatabase& db, const QString& profile, const QString& id) {
    using R=Result<ContextSpec, DomainError>;
    QSqlQuery q(db); q.prepare("SELECT channel,kind,revision FROM agent_conversations WHERE profile_id=? AND id=?");
    q.addBindValue(profile); q.addBindValue(id);
    if (!q.exec()) return R::failure(storeError(q.lastError().text()));
    if (!q.next()) return R::failure(domainError("UNKNOWN_REFERENCE", "Conversation does not exist"));
    ContextSpec context;
    context.profileId=profile; context.conversationId=id; context.channel=q.value(0).toString();
    context.originKind=q.value(1).toString()=="local_owner" ? OriginKind::LocalOwner : OriginKind::External;
    context.conversationRevision=q.value(2).toLongLong();
    q.prepare("SELECT DISTINCT p.person_id,p.revision FROM agent_messages m JOIN agent_persons p "
              "ON p.profile_id=m.profile_id AND p.person_id=m.speaker_id WHERE m.profile_id=? AND m.conversation_id=? "
              "ORDER BY p.person_id");
    q.addBindValue(profile); q.addBindValue(id);
    if (!q.exec()) return R::failure(storeError(q.lastError().text()));
    while (q.next()) { context.participantPersonIds.append(q.value(0).toString()); context.identityRevision+=q.value(1).toLongLong(); }
    return R::success(context);
}
VoidResult validateTarget(QSqlDatabase& db, const QString& profile, const ReplyTarget& target) {
    QSqlQuery q(db); q.prepare("SELECT channel,connection_key,remote_id FROM agent_conversations WHERE profile_id=? AND id=?");
    q.addBindValue(profile); q.addBindValue(target.conversationId);
    if (!q.exec()) return VoidResult::failure(storeError(q.lastError().text()));
    if (!q.next() || q.value(0).toString()!=target.channel || q.value(1).toString()!=target.connectionKey
        || q.value(2).toString()!=target.remoteConversationId)
        return VoidResult::failure(invalid("Response target does not match its conversation"));
    if (!target.replyToMessageId.isEmpty()) {
        q.prepare("SELECT 1 FROM agent_messages WHERE profile_id=? AND conversation_id=? AND id=?");
        q.addBindValue(profile); q.addBindValue(target.conversationId); q.addBindValue(target.replyToMessageId);
        if (!q.exec()) return VoidResult::failure(storeError(q.lastError().text()));
        if (!q.next()) return VoidResult::failure(invalid("Reply references another or unknown conversation message"));
    }
    return VoidResult::success();
}
Result<QList<ChatHistoryEntry>, DomainError> history(QSqlDatabase& db, const QString& profile,
                                                    const QString& conversation, int limit, qint64 beforeRow=std::numeric_limits<qint64>::max()) {
    using R=Result<QList<ChatHistoryEntry>,DomainError>;
    QSqlQuery q(db);
    q.prepare("SELECT id,speaker_id,payload_json,occurred_at FROM (SELECT rowid AS ordering,* FROM agent_messages "
              "WHERE profile_id=? AND conversation_id=? AND rowid<? ORDER BY rowid DESC LIMIT ?) ORDER BY ordering");
    q.addBindValue(profile); q.addBindValue(conversation); q.addBindValue(beforeRow); q.addBindValue(qBound(1,limit,100000));
    if (!q.exec()) return R::failure(storeError(q.lastError().text()));
    QList<ChatHistoryEntry> entries;
    const auto context=readContext(db,profile,conversation);
    if (!context && context.error().code!=QStringLiteral("UNKNOWN_REFERENCE")) return R::failure(context.error());
    QSqlQuery targetQuery(db);targetQuery.prepare("SELECT channel,connection_key,remote_id FROM agent_conversations WHERE profile_id=? AND id=?");
    targetQuery.addBindValue(profile);targetQuery.addBindValue(conversation);
    if(!targetQuery.exec())return R::failure(storeError(targetQuery.lastError().text()));
    ReplyTarget target;
    if(targetQuery.next())target={targetQuery.value(0).toString(),targetQuery.value(1).toString(),conversation,targetQuery.value(2).toString(),{}};
    while (q.next()) {
        auto p=parsedObject(q.value(2)); if (!p) return R::failure(p.error());
        ChatHistoryEntry entry;
        entry.id=q.value(0).toString(); entry.speakerId=q.value(1).toString();
        entry.conversationId=conversation; entry.content=p.value()["text"].toString();
        entry.role=p.value()["role"].toString(p.value()["senderKind"]=="self" ? "assistant" : "user");
        entry.replyToId=p.value()["replyToId"].toString();
        if(context) entry.context=context.value();
        entry.replyTarget=target;entry.replyTarget->replyToMessageId=entry.role==QStringLiteral("user")?entry.id:entry.replyToId;
        entry.timestamp=QDateTime::fromString(q.value(3).toString(),Qt::ISODateWithMs);
        entry.status=chatMessageStatusFromStorageName(p.value()["status"].toString("complete")).value_or(ChatMessageStatus::Failed);
        entry.errorMessage=p.value()["errorMessage"].toString();
        for (const auto& source : p.value()["sourceMessageIds"].toArray()) entry.sourceMessageIds.append(source.toString());
        entries.append(entry);
    }
    return R::success(entries);
}
VoidResult queueObserved(RuntimeUnitOfWork& unit, SqliteEventOutbox& outbox, const QString& profile,
                          const QString& id, const QString& conversation, const QString& speaker,
                          const QJsonObject& payload, const QDateTime& occurredAt, OriginKind kind) {
    EventDraft event;
    event.eventId=payload["originId"].toString(); event.profileId=profile;
    event.type="MessageObserved"; event.source="ConversationStore"; event.occurredAt=occurredAt;
    event.payload={{"message_id",id},{"conversation_id",conversation},{"speaker_id",speaker},
                   {"subject_id",speaker},{"text",payload["text"]}, {"sender_kind",payload["senderKind"]},
                   {"origin_kind",originKindToString(kind)},{"historical_import",payload["historical_import"].toBool()}};
    const auto queued=outbox.enqueue(unit,event);
    return queued ? VoidResult::success() : VoidResult::failure(queued.error());
}
VoidResult insertLocal(QSqlDatabase& db, RuntimeUnitOfWork& unit, SqliteEventOutbox& outbox,
                       const QString& profile, const ChatHistoryEntry& entry, bool historicalImport=false) {
    auto ensured=ensureConversation(db,profile,"local_owner","desktop","local_owner","local_owner",ConversationKind::LocalOwner);
    if (!ensured) return ensured;
    QSqlQuery existing(db); existing.prepare("SELECT conversation_id,payload_json FROM agent_messages WHERE profile_id=? AND id=?");
    existing.addBindValue(profile); existing.addBindValue(entry.id);
    if (!existing.exec()) return VoidResult::failure(storeError(existing.lastError().text()));
    if (existing.next()) {
        auto p=parsedObject(existing.value(1)); if (!p) return VoidResult::failure(p.error());
        if (existing.value(0).toString()!="local_owner" || p.value()["text"].toString()!=entry.content
            || p.value()["role"].toString()!=entry.role)
            return VoidResult::failure(invalid("Message id already identifies different content"));
        return VoidResult::success();
    }
    QJsonObject p={{"text",entry.content},{"role",entry.role},{"senderKind",entry.role=="assistant"?"self":entry.role=="user"?"person":"unknown"},
        {"status",chatMessageStatusStorageName(entry.status)},{"replyToId",entry.replyToId},
        {"sourceMessageIds",QJsonArray::fromStringList(entry.sourceMessageIds)}, {"errorMessage",entry.errorMessage},
        {"originId",stableId("origin",profile,entry.id)},{"historical_import",historicalImport}};
    auto inserted=run(db,"INSERT INTO agent_messages(profile_id,id,conversation_id,speaker_id,direction,payload_json,occurred_at) VALUES(?,?,?,?,?,?,?)",
        {profile,entry.id,QStringLiteral("local_owner"),entry.role=="user"?QVariant("owner"):QVariant(),
         entry.role=="assistant"?"outgoing":"incoming",json(p),entry.timestamp.toUTC().toString(Qt::ISODateWithMs)});
    if (!inserted) return inserted;
    auto event=queueObserved(unit,outbox,profile,entry.id,"local_owner",entry.role=="user"?"owner":"",p,entry.timestamp,OriginKind::LocalOwner);
    if (!event) return event;
    return incrementRevision(db,profile,"local_owner");
}
}

ConversationStore::ConversationStore(QString profileId, QString databasePath)
    : m_profileId(std::move(profileId)),m_databasePath(std::move(databasePath)) {
    HostConversationBinding local;
    local.channel="desktop"; local.connectionKey="local_owner"; local.remoteConversationId="local_owner";
    local.kind=ConversationKind::LocalOwner;
    m_bindings.insert(conversationBindingKey(local.channel,local.connectionKey,local.remoteConversationId),local);
}
VoidResult ConversationStore::initialize() {
    if (!isCanonicalEventUuid(m_profileId) || m_databasePath.isEmpty())
        return VoidResult::failure(invalid("Conversation store requires a profile and runtime path"));
    SqliteEventRepository events; auto opened=events.open(m_databasePath); if (!opened) return opened;
    SqliteIdentityRepository identities; auto identityOpened=identities.open(m_databasePath); if (!identityOpened) return identityOpened;
    SqliteRuntimeUnitOfWorkFactory factory(m_databasePath);
    EventSchemaRegistry schemas; registerBuiltInEventSchemas(schemas);
    SqliteEventOutbox outbox(m_databasePath,&schemas,m_profileId);
    PersonDirectory people(m_profileId,&identities,&factory,&outbox);
    auto initialized=people.initialize(); if (!initialized) return initialized;
    auto begun=factory.begin(); if (!begun) return VoidResult::failure(begun.error());
    auto& unit=*begun.value(); auto db=QSqlDatabase::database(unit.connectionName());
    auto ensured=ensureConversation(db,m_profileId,"local_owner","desktop","local_owner","local_owner",ConversationKind::LocalOwner);
    if (!ensured) return ensured;
    return unit.commit();
}
VoidResult ConversationStore::registerBinding(const HostConversationBinding& b) {
    const bool local=b.channel=="desktop" && b.connectionKey=="local_owner" && b.remoteConversationId=="local_owner";
    if (b.channel.trimmed().isEmpty() || b.connectionKey.trimmed().isEmpty() || b.remoteConversationId.trimmed().isEmpty()
        || (b.kind==ConversationKind::LocalOwner)!=local)
        return VoidResult::failure(invalid("Invalid host conversation binding"));
    m_bindings.insert(conversationBindingKey(b.channel,b.connectionKey,b.remoteConversationId),b);
    return VoidResult::success();
}
Result<AcceptedMessage, DomainError> ConversationStore::acceptMessage(const IncomingMessage& message) {
    using R=Result<AcceptedMessage,DomainError>;
    const auto binding=m_bindings.constFind(conversationBindingKey(message.channel,message.connectionKey,message.remoteConversationId));
    if (binding==m_bindings.cend()) return R::failure(domainError("DEPENDENCY_UNAVAILABLE","Conversation connection is not registered"));
    const bool local=binding->kind==ConversationKind::LocalOwner;
    if (message.kind!=binding->kind || (local ? message.originKind!=OriginKind::LocalOwner : message.originKind!=OriginKind::External)
        || message.content.trimmed().isEmpty() || message.content.size()>8000 || !message.occurredAt.isValid()
        || (local ? message.localMessageId.isEmpty() : (!message.localMessageId.isEmpty()
            || (message.remoteMessageId.isEmpty() && message.sourceEventId.isEmpty()))))
        return R::failure(invalid("Invalid incoming message or origin"));
    const QString conversation=conversationIdFor(m_profileId,message.channel,message.connectionKey,message.remoteConversationId);
    const QString remote=message.remoteMessageId;
    const QString id=local ? message.localMessageId : stableId("message",m_profileId,
        conversation+json(QJsonObject{{"remote",remote},{"event",remote.isEmpty()?message.sourceEventId:QString()}}));
    const bool self=!local && sameIdentity(message.speakerIdentity,binding->selfIdentity);
    EventSchemaRegistry schemas; registerBuiltInEventSchemas(schemas);
    SqliteEventOutbox outbox(m_databasePath,&schemas,m_profileId);
    SqliteRuntimeUnitOfWorkFactory factory(m_databasePath);
    PersonResolution resolution;
    if (!self) {
        SqliteIdentityRepository identities; auto opened=identities.open(m_databasePath); if (!opened) return R::failure(opened.error());
        PersonDirectory people(m_profileId,&identities,&factory,&outbox);
        ResolvePersonRequest request; request.profileId=m_profileId; request.ownerLocal=local;
        if (!local) request.externalIdentity=message.speakerIdentity;
        auto resolved=people.resolvePerson(request); if (!resolved) return R::failure(resolved.error());
        resolution=resolved.value();
    }
    auto begun=factory.begin(); if (!begun) return R::failure(begun.error());
    auto& unit=*begun.value(); auto db=QSqlDatabase::database(unit.connectionName());
    auto ensured=ensureConversation(db,m_profileId,conversation,message.channel,message.connectionKey,message.remoteConversationId,message.kind,message.title);
    if (!ensured) return R::failure(ensured.error());
    QSqlQuery q(db); q.prepare("SELECT id,speaker_id,payload_json FROM agent_messages WHERE profile_id=? AND conversation_id=? AND (id=? OR (?<>'' AND remote_id=?))");
    q.addBindValue(m_profileId); q.addBindValue(conversation); q.addBindValue(id); q.addBindValue(nonnull(remote)); q.addBindValue(nonnull(remote));
    if (!q.exec()) return R::failure(storeError(q.lastError().text()));
    AcceptedMessage accepted; accepted.messageId=id; accepted.conversationId=conversation; accepted.personResolution=resolution;
    accepted.selfMessage=self; accepted.originId=stableId("origin",m_profileId,id);
    if (q.next()) {
        auto p=parsedObject(q.value(2)); if (!p) return R::failure(p.error());
        if (p.value()["text"].toString()!=message.content || q.value(1).toString()!=resolution.personId.value_or(QString())
            || (p.value()["senderKind"]=="self")!=self)
            return R::failure(invalid("Repeated message identity has conflicting content or speaker"));
        accepted.duplicate=true; accepted.messageId=q.value(0).toString(); accepted.originId=p.value()["originId"].toString();
    } else {
        QJsonObject p={{"text",message.content},{"senderKind",self?"self":resolution.personId?"person":"unknown"},
            {"role",self?"assistant":"user"},{"status","complete"},{"replyToRemoteId",message.replyToRemoteId},
            {"originId",accepted.originId},{"sourceEventId",message.sourceEventId},
            {"externalSpeaker",QJsonObject{{"provider",message.speakerIdentity.provider},{"namespace",message.speakerIdentity.nameSpace},{"remoteActorId",message.speakerIdentity.remoteActorId}}}};
        auto inserted=run(db,"INSERT INTO agent_messages(profile_id,id,conversation_id,remote_id,speaker_id,direction,payload_json,occurred_at) VALUES(?,?,?,?,?,?,?,?)",
            {m_profileId,id,conversation,remote.isEmpty()?QVariant():QVariant(remote),resolution.personId?QVariant(*resolution.personId):QVariant(),
             self?"outgoing":"incoming",json(p),message.occurredAt.toUTC().toString(Qt::ISODateWithMs)});
        if (!inserted) return R::failure(inserted.error());
        auto event=queueObserved(unit,outbox,m_profileId,id,conversation,resolution.personId.value_or(QString()),p,message.occurredAt,message.originKind);
        if (!event) return R::failure(event.error());
        auto increased=incrementRevision(db,m_profileId,conversation); if (!increased) return R::failure(increased.error());
    }
    auto context=readContext(db,m_profileId,conversation); if (!context) return R::failure(context.error());
    accepted.context=context.value(); accepted.revision=context.value().conversationRevision;
    accepted.replyTarget={message.channel,message.connectionKey,conversation,message.remoteConversationId,accepted.messageId};
    auto committed=unit.commit(); if (!committed) return R::failure(committed.error());
    outbox.dispatchPending(100); // Message and origin outbox are already committed.
    return R::success(accepted);
}
Result<QList<ChatHistoryEntry>, DomainError> ConversationStore::loadHistory(const QString& conversation,int limit) const {
    auto begun=SqliteRuntimeUnitOfWork::begin(m_databasePath);
    if (!begun) return Result<QList<ChatHistoryEntry>,DomainError>::failure(begun.error());
    auto db=QSqlDatabase::database(begun.value()->connectionName());
    return history(db,m_profileId,conversation,limit);
}
VoidResult ConversationStore::appendLocalHistory(const ChatHistoryEntry& entry) {
    if (entry.id.isEmpty() || !entry.timestamp.isValid() || !isTerminalChatMessageStatus(entry.status)
        || (entry.role!="user" && entry.role!="assistant" && entry.role!="system")
        || (entry.role!="assistant" && (!entry.replyToId.isEmpty() || entry.content.trimmed().isEmpty())))
        return VoidResult::failure(invalid("Invalid terminal chat history entry"));
    EventSchemaRegistry schemas; registerBuiltInEventSchemas(schemas);
    SqliteEventOutbox outbox(m_databasePath,&schemas,m_profileId);
    auto begun=beginConversationWrite(m_databasePath); if (!begun) return VoidResult::failure(begun.error());
    auto db=QSqlDatabase::database(begun.value()->connectionName());
    auto inserted=insertLocal(db,*begun.value(),outbox,m_profileId,entry); if (!inserted) return inserted;
    auto committed=begun.value()->commit(); if (!committed) return committed;
    outbox.dispatchPending(100); return VoidResult::success();
}
VoidResult ConversationStore::importLocalHistory(const QList<ChatHistoryEntry>& entries,const QString& sourceRef) {
    EventSchemaRegistry schemas; registerBuiltInEventSchemas(schemas);
    SqliteEventOutbox outbox(m_databasePath,&schemas,m_profileId);
    QJsonArray sourceSnapshot;
    for (const auto& entry:entries) sourceSnapshot.append(QJsonArray{entry.id,entry.role,entry.content,entry.replyToId,
        QJsonArray::fromStringList(entry.sourceMessageIds),entry.timestamp.toUTC().toString(Qt::ISODateWithMs),int(entry.status)});
    const QString hash=QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(sourceSnapshot).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
    QJsonObject cursor;
    {
        auto begun=beginConversationWrite(m_databasePath); if (!begun) return VoidResult::failure(begun.error());
        auto db=QSqlDatabase::database(begun.value()->connectionName());
        QSqlQuery progress(db);progress.prepare("SELECT cursor_json,state FROM agent_runtime_migrations WHERE profile_id=? AND migration_id='local_history_v2'");
        progress.addBindValue(m_profileId);
        if (!progress.exec()) return VoidResult::failure(storeError(progress.lastError().text()));
        if (progress.next()) {
            if (progress.value(1).toString()=="completed") return VoidResult::success();
            auto parsed=parsedObject(progress.value(0));if(!parsed)return VoidResult::failure(parsed.error());cursor=parsed.value();
            if(cursor["source"].toString()!=sourceRef || cursor["sourceSnapshotRef"].toString()!=hash)
                return VoidResult::failure(domainError("STALE_CONTEXT","History source changed during migration"));
        } else {
            QJsonObject mapping;
            for(const auto& entry:entries) {
                QString id=entry.id;
                QSqlQuery q(db);q.prepare("SELECT conversation_id,payload_json FROM agent_messages WHERE profile_id=? AND id=?");
                q.addBindValue(m_profileId);q.addBindValue(id);if(!q.exec())return VoidResult::failure(storeError(q.lastError().text()));
                if(q.next()) {auto p=parsedObject(q.value(1));if(!p)return VoidResult::failure(p.error());
                    if(q.value(0).toString()!="local_owner" || p.value()["text"].toString()!=entry.content || p.value()["role"].toString()!=entry.role)
                        id=stableId("legacy",m_profileId,sourceRef+":"+entry.id);
                }
                mapping.insert(entry.id,id);
            }
            cursor={{"source",sourceRef},{"sourceSnapshotRef",hash},{"schemaVersion",2},{"nextIndex",0},{"idMapping",mapping}};
            auto created=run(db,"INSERT INTO agent_runtime_migrations(profile_id,migration_id,cursor_json,state) VALUES(?,'local_history_v2',?,'running')",{m_profileId,json(cursor)});
            if(!created)return created;
        }
        auto committed=begun.value()->commit();if(!committed)return committed;
    }
    int next=cursor["nextIndex"].toInt(-1);
    if(next<0 || next>entries.size() || !cursor["idMapping"].isObject())return VoidResult::failure(storeError("Invalid history migration cursor"));
    const auto mapping=cursor["idMapping"].toObject();
    do {
        const int end=qMin(next+100,int(entries.size()));
        auto begun=beginConversationWrite(m_databasePath);if(!begun)return VoidResult::failure(begun.error());
        auto db=QSqlDatabase::database(begun.value()->connectionName());
        for(int i=next;i<end;++i){
            auto entry=entries[i];entry.id=mapping[entry.id].toString(entry.id);
            entry.replyToId=mapping[entry.replyToId].toString(entry.replyToId);
            for(auto& id:entry.sourceMessageIds)id=mapping[id].toString(id);
            auto inserted=insertLocal(db,*begun.value(),outbox,m_profileId,entry,true);if(!inserted)return inserted;
        }
        cursor["nextIndex"]=end;cursor["lastImportedId"]=end>0?entries[end-1].id:QString();
        auto marked=run(db,"UPDATE agent_runtime_migrations SET cursor_json=?,state=?,updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE profile_id=? AND migration_id='local_history_v2'",
            {json(cursor),end==entries.size()?"completed":"running",m_profileId});if(!marked)return marked;
        auto committed=begun.value()->commit();if(!committed)return committed;
        outbox.dispatchPending(100);
        next=end;
    }while(next<entries.size());
    return VoidResult::success();
}
Result<bool, DomainError> ConversationStore::localHistoryMigrationComplete() const {
    using R = Result<bool, DomainError>;
    auto begun = SqliteRuntimeUnitOfWork::begin(m_databasePath);
    if (!begun) return R::failure(begun.error());
    QSqlQuery query(QSqlDatabase::database(begun.value()->connectionName()));
    query.prepare("SELECT state FROM agent_runtime_migrations WHERE profile_id=? AND migration_id='local_history_v2'");
    query.addBindValue(m_profileId);
    if (!query.exec()) return R::failure(storeError(query.lastError().text()));
    return R::success(query.next() && query.value(0).toString() == "completed");
}

Result<PreparedContext, DomainError> ConversationStore::prepareContext(const ContextRequestV2& request) {
    using R=Result<PreparedContext,DomainError>;
    if (request.historyLimit < 1 || request.historyLimit > 1000 || request.requestId.isEmpty() || request.context.profileId!=m_profileId
        || request.context.conversationId!=request.replyTarget.conversationId)
        return R::failure(invalid("Context request is missing its host identity"));
    auto begun=beginConversationWrite(m_databasePath); if (!begun) return R::failure(begun.error());
    auto db=QSqlDatabase::database(begun.value()->connectionName());
    auto valid=validateTarget(db,m_profileId,request.replyTarget); if (!valid) return R::failure(valid.error());
    auto current=readContext(db,m_profileId,request.context.conversationId); if (!current) return R::failure(current.error());
    if (request.context.originKind!=current.value().originKind
        || request.context.channel!=current.value().channel
        || (request.context.conversationRevision>0 && request.context.conversationRevision!=current.value().conversationRevision)
        || (request.context.identityRevision>0 && request.context.identityRevision!=current.value().identityRevision))
        return R::failure(domainError("STALE_CONTEXT","Conversation or identity changed before preparation"));
    PreparedContext prepared;
    prepared.context=current.value(); prepared.context.activityId=request.context.activityId;
    prepared.requestId=request.requestId; prepared.generation=request.generation; prepared.replyTarget=request.replyTarget;
    prepared.sourceMessageIds=request.sourceMessageIds;
    qint64 sourceWatermark=std::numeric_limits<qint64>::max();
    for (const auto& source:request.sourceMessageIds) {
        QSqlQuery q(db); q.prepare("SELECT payload_json,speaker_id,rowid FROM agent_messages WHERE profile_id=? AND conversation_id=? AND id=?");
        q.addBindValue(m_profileId); q.addBindValue(request.context.conversationId); q.addBindValue(source);
        if (!q.exec()) return R::failure(storeError(q.lastError().text()));
        if (!q.next()) return R::failure(invalid("Context source belongs to another or unknown conversation"));
        auto p=parsedObject(q.value(0)); if (!p) return R::failure(p.error());
        prepared.originIds.append(p.value()["originId"].toString());
        prepared.currentInputMessages.append(QJsonObject{{"message_id",source},{"speaker_id",q.value(1).toString()},{"text",p.value()["text"]}});
        sourceWatermark=qMin(sourceWatermark,q.value(2).toLongLong());
    }
    if (!request.replyTarget.replyToMessageId.isEmpty()) {
        QSqlQuery q(db); q.prepare("SELECT speaker_id FROM agent_messages WHERE profile_id=? AND conversation_id=? AND id=?");
        q.addBindValue(m_profileId); q.addBindValue(request.context.conversationId); q.addBindValue(request.replyTarget.replyToMessageId);
        if (!q.exec()) return R::failure(storeError(q.lastError().text()));
        if (q.next()) prepared.subjectPersonId=q.value(0).toString();
    }
    const auto entries=history(db,m_profileId,request.context.conversationId,request.historyLimit,sourceWatermark);
    if (!entries) return R::failure(entries.error());
    for (const auto& entry:entries.value()) {
        if (request.sourceMessageIds.contains(entry.id) || entry.status!=ChatMessageStatus::Complete
            || (entry.role!="user" && entry.role!="assistant") || entry.content.isEmpty()) continue;
        ChatMessage message; message.role=entry.role; message.content=entry.content;
        if (prepared.context.originKind==OriginKind::External)
            message.content=json(QJsonObject{{"message_id",entry.id},{"speaker_id",entry.role=="assistant"?QStringLiteral("self"):entry.speakerId},{"text",entry.content}});
        prepared.messages.append(message);
    }
    while (prepared.messages.size()>request.historyLimit) prepared.messages.removeFirst();
    const QString contextJson=json(contextSpecToJson(prepared.context));
    const QString targetJson=json(request.replyTarget.toJson());
    QSqlQuery q(db); q.prepare("SELECT context_json,target_json,source_ids_json,status FROM agent_response_dispatches WHERE profile_id=? AND request_id=? AND generation=?");
    q.addBindValue(m_profileId); q.addBindValue(request.requestId); q.addBindValue(request.generation);
    if (!q.exec()) return R::failure(storeError(q.lastError().text()));
    if (q.next()) {
        if (q.value(0).toString()!=contextJson || q.value(1).toString()!=targetJson || q.value(2).toString()!=json(request.sourceMessageIds))
            return R::failure(domainError("STALE_CONTEXT","Request already belongs to a different snapshot"));
        if (q.value(3).toString()!="prepared") return R::failure(domainError("STALE_CONTEXT","Request has already been dispatched or cancelled"));
    } else {
        auto cancelled=run(db,"UPDATE agent_response_dispatches SET status='cancelled' WHERE profile_id=? AND conversation_id=? AND status='prepared'",
                           {m_profileId,request.context.conversationId});
        if (!cancelled) return R::failure(cancelled.error());
        auto inserted=run(db,"INSERT INTO agent_response_dispatches(profile_id,request_id,generation,conversation_id,context_json,target_json,source_ids_json) VALUES(?,?,?,?,?,?,?)",
            {m_profileId,request.requestId,request.generation,request.context.conversationId,contextJson,targetJson,json(request.sourceMessageIds)});
        if (!inserted) return R::failure(inserted.error());
    }
    auto committed=begun.value()->commit(); if (!committed) return R::failure(committed.error());
    return R::success(prepared);
}
VoidResult ConversationStore::validateContext(const ContextSpec& context) const {
    if (context.profileId!=m_profileId) return VoidResult::failure(invalid("Context profile mismatch"));
    auto begun=SqliteRuntimeUnitOfWork::begin(m_databasePath); if (!begun) return VoidResult::failure(begun.error());
    auto db=QSqlDatabase::database(begun.value()->connectionName());
    auto current=readContext(db,m_profileId,context.conversationId); if (!current) return VoidResult::failure(current.error());
    if (current.value().conversationRevision!=context.conversationRevision || current.value().identityRevision!=context.identityRevision
        || current.value().participantPersonIds!=context.participantPersonIds || current.value().originKind!=context.originKind)
        return VoidResult::failure(domainError("STALE_CONTEXT","Conversation or identity changed"));
    return VoidResult::success();
}
VoidResult ConversationStore::publishResponse(const RoutedResponse& response) {
    if (response.content.trimmed().isEmpty() || response.content.size()>32000 || response.context.profileId!=m_profileId)
        return VoidResult::failure(invalid("Invalid response content or profile"));
    auto begun=beginConversationWrite(m_databasePath); if (!begun) return VoidResult::failure(begun.error());
    auto db=QSqlDatabase::database(begun.value()->connectionName());
    QSqlQuery q(db); q.prepare("SELECT context_json,target_json,source_ids_json,status FROM agent_response_dispatches WHERE profile_id=? AND request_id=? AND generation=?");
    q.addBindValue(m_profileId); q.addBindValue(response.requestId); q.addBindValue(response.generation);
    if (!q.exec()) return VoidResult::failure(storeError(q.lastError().text()));
    if (!q.next()) return VoidResult::failure(domainError("STALE_CONTEXT","Response request was not prepared"));
    if (q.value(1).toString()!=json(response.replyTarget.toJson()) || q.value(2).toString()!=json(response.sourceMessageIds))
        return VoidResult::failure(invalid("Response does not match its bound target or sources"));
    if (q.value(0).toString()!=json(contextSpecToJson(response.context)) || q.value(3).toString()!="prepared")
        return VoidResult::failure(domainError("STALE_CONTEXT","Response request is stale or already dispatched"));
    auto current=readContext(db,m_profileId,response.context.conversationId); if (!current) return VoidResult::failure(current.error());
    if (current.value().conversationRevision!=response.context.conversationRevision || current.value().identityRevision!=response.context.identityRevision)
        return VoidResult::failure(domainError("STALE_CONTEXT","Conversation changed before sending"));
    auto changed=run(db,"UPDATE agent_response_dispatches SET status='dispatching',content=? WHERE profile_id=? AND request_id=? AND generation=? AND status='prepared'",
                     {response.content,m_profileId,response.requestId,response.generation});
    if (!changed) return changed;
    return begun.value()->commit();
}
VoidResult ConversationStore::cancelRequest(const QString& requestId,quint64 generation,bool beforeDispatch) {
    auto begun=beginConversationWrite(m_databasePath); if (!begun) return VoidResult::failure(begun.error());
    auto db=QSqlDatabase::database(begun.value()->connectionName());
    auto result=run(db,"UPDATE agent_response_dispatches SET status='cancelled' WHERE profile_id=? AND request_id=? AND generation=? AND (status='prepared' OR (? AND status='dispatching'))",
                    {m_profileId,requestId,generation,beforeDispatch});
    if (!result) return result;
    return begun.value()->commit();
}
VoidResult ConversationStore::recordDeliveryReceipt(const DeliveryReceipt& receipt) {
    auto begun=beginConversationWrite(m_databasePath); if (!begun) return VoidResult::failure(begun.error());
    auto& unit=*begun.value(); auto db=QSqlDatabase::database(unit.connectionName());
    QSqlQuery q(db); q.prepare("SELECT target_json,status,content,source_ids_json,receipt_json FROM agent_response_dispatches WHERE profile_id=? AND request_id=? AND generation=?");
    q.addBindValue(m_profileId); q.addBindValue(receipt.requestId); q.addBindValue(receipt.generation);
    if (!q.exec()) return VoidResult::failure(storeError(q.lastError().text()));
    if (!q.next() || q.value(0).toString()!=json(receipt.replyTarget.toJson()))
        return VoidResult::failure(invalid("Receipt is not bound to this dispatched request"));
    const auto status=q.value(1).toString();
    if (status=="prepared" || status=="cancelled") return VoidResult::failure(invalid("Receipt has no actual dispatch"));
    if (status=="completed") {
        auto previous=parsedObject(q.value(4)); if (!previous) return VoidResult::failure(previous.error());
        if (receipt.outcome==DeliveryOutcome::Completed && previous.value()["remote_message_id"].toString()==receipt.remoteMessageId
            && previous.value()["content"].toString()==receipt.content) return VoidResult::success();
        return VoidResult::failure(invalid("Receipt conflicts with a completed dispatch"));
    }
    const auto sourceIds=QJsonDocument::fromJson(q.value(3).toByteArray()).array();
    const bool completed=receipt.outcome==DeliveryOutcome::Completed;
    if (completed && (receipt.remoteMessageId.isEmpty() || receipt.content.isEmpty() || !receipt.occurredAt.isValid()))
        return VoidResult::failure(invalid("A completed send requires an actual message receipt"));
    const QJsonObject receiptJson={{"remote_message_id",receipt.remoteMessageId},{"content",receipt.content},
        {"occurred_at",receipt.occurredAt.toUTC().toString(Qt::ISODateWithMs)},{"evidence",receipt.evidence}};
    EventSchemaRegistry schemas; registerBuiltInEventSchemas(schemas);
    SqliteEventOutbox outbox(m_databasePath,&schemas,m_profileId);
    if (completed) {
        const auto& target=receipt.replyTarget;
        q.prepare("SELECT payload_json,speaker_id FROM agent_messages WHERE profile_id=? AND conversation_id=? AND remote_id=?");
        q.addBindValue(m_profileId); q.addBindValue(target.conversationId); q.addBindValue(receipt.remoteMessageId);
        if (!q.exec()) return VoidResult::failure(storeError(q.lastError().text()));
        if (q.next()) {
            auto p=parsedObject(q.value(0)); if (!p) return VoidResult::failure(p.error());
            if (p.value()["senderKind"]!="self" || !q.value(1).isNull() || p.value()["text"].toString()!=receipt.content)
                return VoidResult::failure(invalid("Remote message id already belongs to another message"));
            auto payload=p.value();
            const auto oldReply=payload["replyToId"].toString();
            const auto oldSources=payload["sourceMessageIds"].toArray();
            if ((!oldReply.isEmpty() && oldReply!=target.replyToMessageId)
                || (!oldSources.isEmpty() && oldSources!=sourceIds))
                return VoidResult::failure(invalid("Self echo contains conflicting reply references"));
            payload["replyToId"]=target.replyToMessageId;payload["sourceMessageIds"]=sourceIds;
            auto linked=run(db,"UPDATE agent_messages SET payload_json=? WHERE profile_id=? AND conversation_id=? AND remote_id=?",
                           {json(payload),m_profileId,target.conversationId,receipt.remoteMessageId});
            if(!linked)return linked;
        } else {
            const auto id=stableId("message",m_profileId,target.conversationId+json(QJsonObject{{"remote",receipt.remoteMessageId},{"event",QString()}}));
            QJsonObject p={{"text",receipt.content},{"senderKind","self"},{"role","assistant"},{"status","complete"},
                {"replyToId",target.replyToMessageId},{"sourceMessageIds",sourceIds},{"originId",stableId("origin",m_profileId,id)}};
            auto inserted=run(db,"INSERT INTO agent_messages(profile_id,id,conversation_id,remote_id,direction,payload_json,occurred_at) VALUES(?,?,?,?,'outgoing',?,?)",
                {m_profileId,id,target.conversationId,receipt.remoteMessageId,json(p),receipt.occurredAt.toUTC().toString(Qt::ISODateWithMs)});
            if (!inserted) return inserted;
            auto event=queueObserved(unit,outbox,m_profileId,id,target.conversationId,{},p,receipt.occurredAt,OriginKind::External);
            if (!event) return event;
            auto increased=incrementRevision(db,m_profileId,target.conversationId); if (!increased) return increased;
        }
    }
    auto updated=run(db,"UPDATE agent_response_dispatches SET status=?,receipt_json=?,updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE profile_id=? AND request_id=? AND generation=?",
        {completed?"completed":receipt.outcome==DeliveryOutcome::NotExecuted?"failed":"unknown",json(receiptJson),m_profileId,receipt.requestId,receipt.generation});
    if (!updated) return updated;
    auto committed=unit.commit(); if (!committed) return committed;
    outbox.dispatchPending(100);
    return VoidResult::success();
}

Result<int, DomainError> ConversationStore::recoverOutbox() {
    EventSchemaRegistry schemas;registerBuiltInEventSchemas(schemas);
    SqliteEventOutbox outbox(m_databasePath,&schemas,m_profileId);
    const auto delivered=outbox.dispatchPending(100);
    if(!delivered)return Result<int,DomainError>::failure(delivered.error());
    auto begun=SqliteRuntimeUnitOfWork::begin(m_databasePath);if(!begun)return Result<int,DomainError>::failure(begun.error());
    QSqlQuery q(QSqlDatabase::database(begun.value()->connectionName()));
    q.prepare("SELECT count(*) FROM event_outbox WHERE status='Pending'");
    if(!q.exec() || !q.next())return Result<int,DomainError>::failure(storeError(q.lastError().text()));
    return Result<int,DomainError>::success(q.value(0).toInt());
}
