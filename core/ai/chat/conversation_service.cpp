#include "conversation_service.h"
#include <QSqlDatabase>
#include <QSqlQuery>
#include "ai/event/runtime_unit_of_work.h"

ConversationService::ConversationService(QString profile,QString path,QObject* parent)
    : QObject(parent),m_store(std::move(profile),std::move(path)) {
    m_worker.setMaxThreadCount(1);
    m_outboxRetryTimer.setInterval(1000);
    connect(&m_outboxRetryTimer,&QTimer::timeout,this,[this]{
        if(m_recoveringOutbox)return;
        m_recoveringOutbox=true;
        enqueue<int>([](ConversationStore& store){return store.recoverOutbox();},[this](Result<int,DomainError> result){
            m_recoveringOutbox=false;
            if(result && result.value()==0)m_outboxRetryTimer.stop();
        });
    });
    m_outboxRetryTimer.start();
}
ConversationService::~ConversationService() { m_outboxRetryTimer.stop(); m_worker.waitForDone(); }
Result<void,DomainError> ConversationService::registerBinding(const HostConversationBinding& binding) {
    auto result=m_store.registerBinding(binding);
    if (result) m_bindings.insert(conversationBindingKey(binding.channel,binding.connectionKey,binding.remoteConversationId),binding);
    return result;
}
void ConversationService::acceptMessageAsync(IncomingMessage message,Completion<AcceptedMessage> completion) {
    enqueue<AcceptedMessage>([message=std::move(message)](ConversationStore& store){return store.acceptMessage(message);},std::move(completion));
}
void ConversationService::prepareContextAsync(ContextRequestV2 request,Completion<PreparedContext> completion) {
    enqueue<PreparedContext>([request=std::move(request)](ConversationStore& store){return store.prepareContext(request);},std::move(completion));
}
void ConversationService::recordDeliveryReceiptAsync(DeliveryReceipt receipt,Completion<void> completion) {
    enqueue<void>([receipt=std::move(receipt)](ConversationStore& store){return store.recordDeliveryReceipt(receipt);},std::move(completion));
}
void ConversationService::cancelRequestAsync(QString requestId,quint64 generation) {
    const QString key=requestId+QLatin1Char(':')+QString::number(generation);
    const auto token=m_dispatchTokens.value(key);
    const bool beforeDispatch=token && !token->started;
    if(beforeDispatch) token->cancelled=true;
    enqueue<void>([requestId=std::move(requestId),generation,beforeDispatch](ConversationStore& store){return store.cancelRequest(requestId,generation,beforeDispatch);},{});
}
void ConversationService::publishResponseAsync(RoutedResponse response,Completion<void> completion) {
    const auto key=conversationBindingKey(response.replyTarget.channel,response.replyTarget.connectionKey,response.replyTarget.remoteConversationId);
    const auto found=m_bindings.constFind(key);
    if (found==m_bindings.cend() || !found->dispatcher || found->toolName.isEmpty()) {
        completion(Result<void,DomainError>::failure(domainError("DEPENDENCY_UNAVAILABLE","Conversation has no asynchronous sender")));
        return;
    }
    const auto binding=*found;
    const QString tokenKey=response.requestId+QLatin1Char(':')+QString::number(response.generation);
    if (m_dispatchTokens.contains(tokenKey)) {
        completion(Result<void,DomainError>::failure(domainError("STALE_CONTEXT","Response is already queued for dispatch")));
        return;
    }
    const auto token=std::make_shared<DispatchToken>();
    m_dispatchTokens.insert(tokenKey,token);
    QPointer<ConversationService> guard(this);
    enqueue<ReplyDispatch>([response=std::move(response),binding](ConversationStore& store) {
        using R=Result<ReplyDispatch,DomainError>;
        QString remoteReply;
        if (!response.replyTarget.replyToMessageId.isEmpty()) {
            auto begun=SqliteRuntimeUnitOfWork::begin(store.databasePath()); if (!begun) return R::failure(begun.error());
            auto db=QSqlDatabase::database(begun.value()->connectionName());
            QSqlQuery q(db);q.prepare("SELECT remote_id FROM agent_messages WHERE profile_id=? AND conversation_id=? AND id=?");
            q.addBindValue(store.profileId());q.addBindValue(response.context.conversationId);q.addBindValue(response.replyTarget.replyToMessageId);
            if (!q.exec() || !q.next()) return R::failure(domainError("STORE_UNAVAILABLE","Unable to resolve response reference"));
            remoteReply=q.value(0).toString();
        }
        auto reserved=store.publishResponse(response); if (!reserved) return R::failure(reserved.error());
        ReplyDispatch dispatch; dispatch.response=response;dispatch.toolName=binding.toolName;
        dispatch.arguments={{"connection_key",response.replyTarget.connectionKey},{"conversation_id",response.replyTarget.remoteConversationId},
                            {"reply_to_message_id",remoteReply},{"text",response.content}};
        return R::success(dispatch);
    },[guard,binding,token,tokenKey,completion=std::move(completion)](Result<ReplyDispatch,DomainError> prepared) mutable {
        if (!guard) return;
        if (!prepared) {guard->m_dispatchTokens.remove(tokenKey);completion(Result<void,DomainError>::failure(prepared.error()));return;}
        const auto dispatch=prepared.value();
        emit guard->replyDispatchPrepared(dispatch.response.requestId, dispatch.response.generation);
        if(token->cancelled) {
            guard->m_dispatchTokens.remove(tokenKey);
            completion(Result<void,DomainError>::failure(domainError("STALE_CONTEXT","Response cancelled before dispatch")));
            return;
        }
        token->started=true;
        auto delivered=std::make_shared<std::atomic_bool>(false);
        ReplyCompletion callback=[guard,dispatch,completion,delivered,tokenKey](DeliveryReceipt receipt) mutable {
            if (!guard) return;
            // Routing identity belongs to the dispatch, never the returned payload.
            receipt.requestId=dispatch.response.requestId;receipt.generation=dispatch.response.generation;
            receipt.replyTarget=dispatch.response.replyTarget;
            QMetaObject::invokeMethod(guard,[guard,receipt=std::move(receipt),completion,delivered,tokenKey]() mutable {
                if (!guard) return;
                guard->m_dispatchTokens.remove(tokenKey);
                const auto outcome=receipt.outcome;
                guard->recordDeliveryReceiptAsync(std::move(receipt),[completion,delivered,outcome](Result<void,DomainError> result) mutable {
                    if (delivered->exchange(true)) return;
                    if (result && outcome!=DeliveryOutcome::Completed)
                        result=Result<void,DomainError>::failure(domainError(outcome==DeliveryOutcome::Unknown?"ACTION_OUTCOME_UNKNOWN":"ACTION_NOT_EXECUTED","Message delivery did not complete"));
                    completion(std::move(result));
                });
            },Qt::QueuedConnection);
        };
        try { binding.dispatcher(dispatch,callback); }
        catch (...) { DeliveryReceipt receipt;receipt.outcome=DeliveryOutcome::Unknown;callback(receipt); }
    });
}

void ConversationService::appendLocalHistoryAsync(ChatHistoryEntry entry,Completion<void> completion) {
    enqueue<void>([entry=std::move(entry)](ConversationStore& store){return store.appendLocalHistory(entry);},std::move(completion));
}
