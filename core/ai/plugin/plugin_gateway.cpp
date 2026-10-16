#include "plugin_runtime.h"
#include "schema_validator.h"
#include "ai/tool_registry.h"
#include <QPointer>
#include <QJsonDocument>
#include <QUuid>
#include <QCoreApplication>
namespace {
bool insertArgument(QJsonObject& root,const QString& pointer,const QJsonValue& value) {
    if (!pointer.startsWith('/') || pointer.size()<2) return false;
    auto parts=pointer.mid(1).split('/');
    for (auto& part:parts) {
        for (int i=0;i<part.size();++i) if (part[i]=='~' && (i+1>=part.size() || (part[i+1]!='0' && part[i+1]!='1'))) return false;
        part.replace("~1","/"); part.replace("~0","~");
    }
    std::function<bool(QJsonObject&,int)> assign=[&](QJsonObject& object,int index) {
        const auto key=parts[index];
        if (index==parts.size()-1) { if (object.contains(key)) return false; object[key]=value; return true; }
        if (object.contains(key) && !object[key].isObject()) return false;
        auto child=object[key].toObject(); if (!assign(child,index+1)) return false; object[key]=child; return true;
    };
    return assign(root,0);
}
QStringList gatewayNames() { return {"discover_capabilities","load_capability","invoke_capability"}; }
class GatewayTool final : public AITool {
public:
    explicit GatewayTool(const QString& name):AITool(name,name=="discover_capabilities"?"Discover installed skills and tools by summary. Load a selected entry before use.":name=="load_capability"?"Load only the selected skill instructions or tool operation schema. Use a reference from discovery.":"Invoke a previously loaded operation using its exact arguments. Host permissions are checked.") {}
    QJsonObject parameterSchema() const override {
        QJsonObject properties; QJsonArray required;
        if (name()!="discover_capabilities") {
            properties["ref"]=QJsonObject{{"type","object"},{"properties",QJsonObject{{"pluginId",QJsonObject{{"type","string"}}},{"entryId",QJsonObject{{"type","string"}}},{"entryVersion",QJsonObject{{"type","string"}}},{"operationId",QJsonObject{{"type","string"}}}}},{"required",QJsonArray{"pluginId","entryId","entryVersion"}},{"additionalProperties",false}};
            required.append("ref");
        }
        if (name()=="invoke_capability") { properties["arguments"]=QJsonObject{{"type","object"}}; required.append("arguments"); }
        return {{"type","object"},{"properties",properties},{"required",required},{"additionalProperties",false}};
    }
    ToolResult execute(const QJsonObject&) override { return ToolResult::fail("Capability gateway requires asynchronous execution"); }
};
}
void PluginRuntime::setLearnedSkills(QList<SkillEntry> snapshot) { QMutexLocker lock(&m_state->mutex); m_state->learned=std::move(snapshot); }
void PluginRuntime::attach(ToolRegistry* registry) {
    if (!registry) return;
    QPointer<PluginRuntime> guard(this);
    for (const auto& name:gatewayNames()) {
        auto metadata=HostToolMetadata::safeRead(HostToolExecutor::Async); metadata.businessEvidence=false;
        if (name=="invoke_capability") metadata.effect="conditional";
        registry->registerTool(std::make_unique<GatewayTool>(name),metadata,{},[guard](const ToolExecutionRequest& request,ToolRuntime::Completion completion) {
            if (!guard) { ToolExecutionOutcome out; out.requestId=request.requestId; out.toolName=request.toolName; out.result=ToolResult::fail("Capability runtime unavailable"); completion(out); return; }
            guard->executeGateway(request,std::move(completion));
        });
    }
}
QJsonArray PluginRuntime::gatewaySchemas(const ContextSpec& context) const {
    QJsonArray result; if (discoverCapabilities(context).isEmpty()) return result;
    for (const auto& name:gatewayNames()) result.append(GatewayTool(name).toFunctionSchema()); return result;
}
bool PluginRuntime::gatewayReadOnly(const QString& name,const QJsonObject& arguments,const ContextSpec& context) const {
    if (name=="discover_capabilities" || name=="load_capability") return true;
    if (name!="invoke_capability") return false;
    const auto ref=CapabilityRef::fromJson(arguments["ref"].toObject());
    QMutexLocker lock(&m_state->mutex); const auto loaded=m_state->loaded.value(context.cacheKey()+ref.key());
    return loaded.catalogRevision==m_state->catalog->revision() && loaded.operation["effects"].toObject()["kind"]=="read";
}
void PluginRuntime::executeGateway(const ToolExecutionRequest& request,ToolRuntime::Completion completion) {
    auto finish=[request,completion=std::move(completion)](ToolResult result,bool executed=false) mutable {
        ToolExecutionOutcome outcome; outcome.requestId=request.requestId; outcome.toolName=request.toolName; outcome.result=std::move(result); outcome.executed=executed;
        outcome.policyDecision=ToolPolicyDecision::allow(ToolRiskLevel::L0SafeRead,"Host capability grant checked"); completion(std::move(outcome));
    };
    GatewayTool definition(request.toolName);
    auto schema=SchemaValidator::validate(definition.parameterSchema(),request.arguments);
    if (!schema) { finish(ToolResult::fail(schema.error().code)); return; }
    if (request.toolName=="invoke_capability") {
        const auto ref=CapabilityRef::fromJson(request.arguments["ref"].toObject()); LoadedCapability loaded;
        { QMutexLocker lock(&m_state->mutex); loaded=m_state->loaded.value(request.context.cacheKey()+ref.key()); }
        auto permission=grant(ref,request.context); if (!permission) { finish(ToolResult::fail(permission.error().code)); return; }
        CapabilityCall call; call.callId=request.requestId; call.activityId=request.activityId; call.context=request.context; call.ref=ref;
        call.arguments=request.arguments["arguments"].toObject(); call.catalogRevision=loaded.catalogRevision; call.definitionDigest=loaded.definitionDigest;
        call.grantRef=permission.value()["id"].toString(); call.timeoutMs=request.timeoutMs>0?request.timeoutMs:30000; call.autonomous=request.autonomous; call.dispatchToken=request.dispatchToken;
        invokeCapability(call,[finish=std::move(finish)](CapabilityOutcome outcome) mutable {
            ToolResult result; result.success=outcome.stage=="completed" || outcome.stage=="accepted"; result.data=outcome.toJson(); result.errorMessage=outcome.error.code;
            finish(std::move(result),outcome.executed);
        }); return;
    }
    QPointer<PluginRuntime> guard(this);
    m_worker.start([this,guard,request,finish=std::move(finish)]() mutable {
        ToolResult result;
        bool cancelled=false; { QMutexLocker lock(&m_state->mutex); cancelled=m_state->stopping || (request.dispatchToken && !request.dispatchToken->allowed()); }
        if (cancelled) { finish(ToolResult::fail("CANCELLED")); return; }
        if (request.toolName=="discover_capabilities") result=ToolResult::ok({{"entries",discoverCapabilities(request.context)}});
        else {
            auto loaded=loadForDispatch(CapabilityRef::fromJson(request.arguments["ref"].toObject()),request.context,request.dispatchToken);
            result=loaded?ToolResult::ok(publicValue(loaded.value().toPublicJson(),loaded.value()).toObject()):ToolResult::fail(loaded.error().code);
        }
        // Completion retains only immutable request data, and ToolRuntime routes owner callbacks.
        finish(std::move(result),true);
    });
}
ReplyDispatcher PluginRuntime::makeReplyDispatcher(const CapabilityReplyBinding& replyBinding) {
    QPointer<PluginRuntime> guard(this);
    return [guard,replyBinding](const ReplyDispatch& dispatch,ReplyCompletion completion) {
        DeliveryReceipt receipt; receipt.requestId=dispatch.response.requestId; receipt.generation=dispatch.response.generation;
        receipt.replyTarget=dispatch.response.replyTarget;
        if (!guard) { receipt.outcome=DeliveryOutcome::NotExecuted; completion(receipt); return; }
        // Load + invoke are chained asynchronously; no GUI-thread definition/MCP reads.
        ToolExecutionRequest load; load.requestId=receipt.requestId+":load"; load.toolName="load_capability";
        load.dispatchToken=dispatch.dispatchToken;
        load.context=dispatch.response.context; load.activityId=dispatch.response.context.activityId;
        load.arguments={{"ref",replyBinding.ref.toJson()}};
        guard->executeGateway(load,[guard,replyBinding,dispatch,receipt,completion=std::move(completion)](ToolExecutionOutcome loaded) mutable {
            if (!guard || !loaded.result.success) { receipt.outcome=DeliveryOutcome::NotExecuted; completion(receipt); return; }
            QMetaObject::invokeMethod(QCoreApplication::instance(),[guard,replyBinding,dispatch,receipt,loaded,completion=std::move(completion)]() mutable {
                if (!guard) { receipt.outcome=DeliveryOutcome::NotExecuted; completion(receipt); return; }
                CapabilityCall call; call.callId=receipt.requestId+":send:"+QString::number(receipt.generation); call.activityId=dispatch.response.context.activityId.isEmpty()?receipt.requestId:dispatch.response.context.activityId;
                call.context=dispatch.response.context; call.dispatchToken=dispatch.dispatchToken; call.ref=replyBinding.ref; call.grantRef=replyBinding.grantRef;
                call.catalogRevision=loaded.result.data["catalogRevision"].toInteger(); call.definitionDigest=loaded.result.data["definitionDigest"].toString();
                for (auto it=replyBinding.argumentBindings.begin();it!=replyBinding.argumentBindings.end();++it) {
                    QJsonValue value;
                    if (it.value()=="content") value=dispatch.response.content;
                    else if (it.value()=="remoteConversationId") value=dispatch.response.replyTarget.remoteConversationId;
                    else if (it.value()=="replyToRemoteId") value=dispatch.arguments.value("reply_to_message_id");
                    else { receipt.outcome=DeliveryOutcome::NotExecuted; completion(receipt); return; }
                    if (!insertArgument(call.arguments,it.key(),value)) { receipt.outcome=DeliveryOutcome::NotExecuted; completion(receipt); return; }
                }
                guard->invokeCapability(call,[replyBinding,receipt,completion=std::move(completion)](CapabilityOutcome outcome) mutable {
                    receipt.evidence=outcome.toJson(); auto id=CapabilityExecutor::pointer(outcome.data,replyBinding.remoteMessageIdPointer);
                    const auto content=CapabilityExecutor::pointer(outcome.data,replyBinding.contentPointer);
                    const auto occurred=CapabilityExecutor::pointer(outcome.data,replyBinding.occurredAtPointer);
                    const auto actualTime=occurred?QDateTime::fromString(occurred.value().toString(),Qt::ISODateWithMs):QDateTime{};
                    if (outcome.stage=="completed" && id && id.value().isString() && !id.value().toString().isEmpty()
                        && content && content.value().isString() && actualTime.isValid()) {
                        receipt.outcome=DeliveryOutcome::Completed; receipt.remoteMessageId=id.value().toString();
                        receipt.content=content.value().toString(); receipt.occurredAt=actualTime;
                    } else receipt.outcome=outcome.executed?DeliveryOutcome::Unknown:DeliveryOutcome::NotExecuted;
                    completion(std::move(receipt));
                });
            },Qt::QueuedConnection);
        });
    };
}

QList<HostConversationBinding> PluginRuntime::conversationBindings() {
    QJsonArray configured; { QMutexLocker lock(&m_state->mutex); configured=m_state->config["replyBindings"].toArray(); }
    QList<HostConversationBinding> result;
    for (const auto& value:configured) {
        const auto object=value.toObject(); HostConversationBinding binding;
        binding.channel=object["channel"].toString(); binding.connectionKey=object["connectionKey"].toString(); binding.remoteConversationId=object["remoteConversationId"].toString();
        binding.kind=object["kind"]=="group"?ConversationKind::Group:ConversationKind::Direct;
        const auto self=object["selfIdentity"].toObject(); binding.selfIdentity.provider=self["provider"].toString(); binding.selfIdentity.nameSpace=self["namespace"].toString(); binding.selfIdentity.remoteActorId=self["remoteActorId"].toString();
        CapabilityReplyBinding reply; reply.ref=CapabilityRef::fromJson(object["ref"].toObject()); reply.grantRef=object["grantRef"].toString();
        reply.argumentBindings=object["argumentBindings"].toObject(); reply.remoteMessageIdPointer=object["remoteMessageIdPointer"].toString("/remote_message_id");
        reply.contentPointer=object["contentPointer"].toString("/content"); reply.occurredAtPointer=object["occurredAtPointer"].toString("/occurred_at");
        binding.toolName="invoke_capability"; binding.dispatcher=makeReplyDispatcher(reply); result.append(binding);
    }
    return result;
}
