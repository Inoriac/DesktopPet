#pragma once
#include "plugin_catalog.h"
#include "capability_executor.h"
#include "ai/skill/skill_types.h"
#include "ai/chat/conversation_types.h"
#include "ai/tools/runtime/tool_runtime.h"
#include <QObject>
#include <QThreadPool>
#include <QMutex>
#include <memory>
#include <optional>
struct CapabilityReplyBinding {
    CapabilityRef ref;
    QString grantRef;
    QJsonObject argumentBindings;
    QString remoteMessageIdPointer = "/remote_message_id";
    QString contentPointer = "/content";
    QString occurredAtPointer = "/occurred_at";
};
class ToolRegistry;
class PluginRuntime final : public QObject {
    Q_OBJECT
public:
    PluginRuntime(QString profileId,QString settingsPath,QObject* parent=nullptr);
    ~PluginRuntime() override;
    Result<void,DomainError> initialize();
    Result<void,DomainError> setHostConfiguration(const QJsonObject& configuration);
    Result<PluginRegistration,DomainError> registerPlugin(const PluginLocation& location);
    Result<LoadedCapability,DomainError> loadCapability(const CapabilityRef& ref,const ContextSpec& context);
    void invokeCapability(const CapabilityCall& call,CapabilityCompletion completion);
    Result<void,DomainError> setPluginEnabled(const PluginStateChange& change);
    QJsonArray discoverCapabilities(const ContextSpec& context) const;
    quint64 catalogRevision() const;
    void attach(ToolRegistry* registry);
    void retire();
    QList<HostConversationBinding> conversationBindings();
    QJsonArray gatewaySchemas(const ContextSpec& context) const;
    void setLearnedSkills(QList<SkillEntry> snapshot);
    bool gatewayReadOnly(const QString& name,const QJsonObject& arguments,const ContextSpec& context) const;
    ReplyDispatcher makeReplyDispatcher(const CapabilityReplyBinding& binding);
    void executeGateway(const ToolExecutionRequest& request,ToolRuntime::Completion completion);
signals:
    void capabilityFinished(const QJsonObject& outcome);
    void capabilityDispatching(const QString& callId);
private:
    struct Pending { CapabilityCompletion completion; std::optional<CapabilityOutcome> outcome; bool settled=false; };
    struct State { QMutex mutex; QJsonObject config; QHash<QString,LoadedCapability> loaded; QList<SkillEntry> learned; std::shared_ptr<PluginCatalog> catalog=std::make_shared<PluginCatalog>(); bool stopping=false; };
    Result<void,DomainError> save(const QJsonObject& config) const;
    Result<QJsonObject,DomainError> grant(const CapabilityRef& ref,const ContextSpec& context,const QString& id={}) const;
    Result<void,DomainError> authorize(const LoadedCapability& loaded,const CapabilityCall& call,const QJsonObject& grant,const CapabilityBinding& binding) const;
    Result<LoadedCapability,DomainError> loadForDispatch(const CapabilityRef& ref,const ContextSpec& context,const std::shared_ptr<ToolDispatchToken>& token);
    CapabilityBinding binding(const CapabilityRef& ref) const;
    QJsonValue publicValue(const QJsonValue& value,const LoadedCapability& loaded) const;
    void settle(const std::shared_ptr<Pending>& pending);
    QString m_profileId,m_settingsPath;
    std::shared_ptr<State> m_state=std::make_shared<State>();
    QThreadPool m_worker;
    QList<std::shared_ptr<Pending>> m_pending;
};
