#include "plugin_runtime.h"
#include "learned_skill_adapter.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSaveFile>
#include <QJsonDocument>
#include <QPointer>
#include <QTimer>
#include <QSet>

namespace {
template<class T> Result<T,DomainError> fail(const QString& code,const QString& message) { return Result<T,DomainError>::failure(domainError(code,message)); }
CapabilityOutcome errorOutcome(const CapabilityCall& call,const DomainError& error) { CapabilityOutcome out; out.callId=call.callId; out.activityId=call.activityId; out.error=error; return out; }
QString canonicalPath(const QString& path) {
    QFileInfo info(path); QStringList missing;
    while (!info.exists() && !info.isSymLink()) { if (info.fileName().isEmpty()) return {}; missing.prepend(info.fileName()); auto parent=info.absolutePath(); if (parent==info.absoluteFilePath()) return {}; info.setFile(parent); }
    auto resolved=info.canonicalFilePath(); if (resolved.isEmpty()) return {};
    for (const auto& part:missing) resolved=QDir(resolved).filePath(part);
    return QDir::cleanPath(resolved);
}
}
PluginRuntime::PluginRuntime(QString profile,QString settings,QObject* parent):QObject(parent),m_profileId(std::move(profile)),m_settingsPath(std::move(settings)) { m_worker.setMaxThreadCount(1); }
PluginRuntime::~PluginRuntime() {
    { QMutexLocker lock(&m_state->mutex); m_state->stopping=true; }
    m_worker.waitForDone();
    const auto pending=m_pending; for (const auto& call:pending) settle(call);
}
Result<void,DomainError> PluginRuntime::save(const QJsonObject& config) const {
    if (m_settingsPath.isEmpty()) return fail<void>("STORE_UNAVAILABLE","Plugin settings path is not configured");
    if (!QDir().mkpath(QFileInfo(m_settingsPath).absolutePath())) return fail<void>("STORE_UNAVAILABLE","Plugin settings directory unavailable");
    QSaveFile file(m_settingsPath);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(config).toJson())<0 || !file.commit()) return fail<void>("STORE_UNAVAILABLE","Plugin settings could not be saved");
    return Result<void,DomainError>::success();
}
Result<void,DomainError> PluginRuntime::initialize() {
    QFile file(m_settingsPath);
    if (!file.exists()) return Result<void,DomainError>::success();
    if (!file.open(QIODevice::ReadOnly) || file.size()>2*1024*1024) return fail<void>("STORE_UNAVAILABLE","Plugin settings unavailable");
    QJsonParseError error; auto doc=QJsonDocument::fromJson(file.readAll(),&error);
    if (error.error!=QJsonParseError::NoError || !doc.isObject()) return fail<void>("INVALID_INPUT","Invalid plugin host configuration");
    return setHostConfiguration(doc.object());
}
Result<void,DomainError> PluginRuntime::setHostConfiguration(const QJsonObject& config) {
    auto normalized=config; QJsonArray normalizedPlugins;
    QSet<QString> pluginIds, grantIds;
    auto catalog=std::make_shared<PluginCatalog>();
    for (const auto& value:config["plugins"].toArray()) {
        const auto plugin=value.toObject(); auto registered=catalog->registerPlugin({plugin["rootPath"].toString()});
        if (!registered) return Result<void,DomainError>::failure(registered.error());
        if (pluginIds.contains(registered.value().pluginId)) return fail<void>("INVALID_INPUT","Duplicate plugin identity in host configuration");
        pluginIds.insert(registered.value().pluginId);
        auto indexed=plugin; indexed["pluginId"]=registered.value().pluginId; normalizedPlugins.append(indexed);
        if (!plugin["enabled"].toBool(true)) catalog->setPluginEnabled({registered.value().pluginId,false,catalog->revision()});
    }
    for (const auto& value:config["grants"].toArray()) {
        const auto grant=value.toObject();
        if (grant["id"].toString().isEmpty() || grant["pluginId"].toString().isEmpty() || grant["profileId"]!=m_profileId || grant["conversationId"].toString().isEmpty() || grant["entryId"].toString().isEmpty())
            return fail<void>("INVALID_INPUT","Invalid host capability grant");
        if (grantIds.contains(grant["id"].toString())) return fail<void>("INVALID_INPUT","Duplicate host grant identity");
        grantIds.insert(grant["id"].toString());
    }
    normalized["plugins"]=normalizedPlugins;
    QMutexLocker lock(&m_state->mutex);
    auto saved=save(normalized); if (!saved) return saved;
    // Preserve a monotonic generation through complete settings replacement.
    while (catalog->revision()<=m_state->catalog->revision()) catalog->invalidate();
    m_state->catalog=catalog; m_state->config=normalized; m_state->loaded.clear();
    return Result<void,DomainError>::success();
}
Result<PluginRegistration,DomainError> PluginRuntime::registerPlugin(const PluginLocation& location) {
    PluginCatalog probe; auto checked=probe.registerPlugin(location); if (!checked) return checked;
    QJsonObject config; { QMutexLocker lock(&m_state->mutex); config=m_state->config; }
    QJsonArray plugins; bool enabled=true;
    for (const auto& value:config["plugins"].toArray()) {
        if (value.toObject()["pluginId"]==checked.value().pluginId) enabled=value.toObject()["enabled"].toBool(true);
        else plugins.append(value);
    }
    plugins.append(QJsonObject{{"rootPath",location.rootPath},{"pluginId",checked.value().pluginId},{"enabled",enabled}}); config["plugins"]=plugins;
    auto saved=setHostConfiguration(config); if (!saved) return Result<PluginRegistration,DomainError>::failure(saved.error());
    checked.value().catalogRevision=catalogRevision(); return checked;
}

quint64 PluginRuntime::catalogRevision() const { QMutexLocker lock(&m_state->mutex); return m_state->catalog->revision(); }
Result<QJsonObject,DomainError> PluginRuntime::grant(const CapabilityRef& ref,const ContextSpec& context,const QString& id) const {
    if (context.profileId!=m_profileId) return fail<QJsonObject>("INVALID_INPUT","Capability context has the wrong profile");
    QMutexLocker lock(&m_state->mutex);
    for (const auto& value:m_state->config["grants"].toArray()) {
        const auto g=value.toObject();
        const bool scope=g["conversationId"]=="local_owner" ? context.originKind==OriginKind::LocalOwner : g["conversationId"]==context.conversationId;
        if (g["profileId"]==context.profileId && g["pluginId"]==ref.pluginId && scope && g["entryId"]==ref.entryId && (id.isEmpty() || g["id"]==id)
            && (ref.operationId.isEmpty() || g["operationId"]==ref.operationId)) return Result<QJsonObject,DomainError>::success(g);
    }
    return fail<QJsonObject>("PERMISSION_DENIED","No host grant matches this capability and conversation");
}
CapabilityBinding PluginRuntime::binding(const CapabilityRef& ref) const {
    QMutexLocker lock(&m_state->mutex);
    return {m_state->config["configuration"].toObject()[ref.entryId].toObject(),m_state->config["connections"].toObject(),
        m_state->config["resultAdapters"].toObject()[ref.entryId+"/"+ref.operationId].toObject()};
}
QJsonArray PluginRuntime::discoverCapabilities(const ContextSpec& context) const {
    std::shared_ptr<PluginCatalog> catalog; { QMutexLocker lock(&m_state->mutex); catalog=m_state->catalog; }
    QJsonArray result; if (context.profileId!=m_profileId) return result;
    for (const auto& value:catalog->discover()) {
        auto entry=value.toObject(); const auto ref=CapabilityRef{entry["pluginId"].toString(),entry["id"].toString(),entry["version"].toString(),{}};
        if (context.originKind==OriginKind::LocalOwner || grant(ref,context)) { entry["catalogRevision"]=qint64(catalog->revision()); result.append(entry); }
    }
    { QMutexLocker lock(&m_state->mutex); for (const auto& skill:LearnedSkillAdapter::discover(m_state->learned,context)) result.append(skill); }
    return result;
}
Result<LoadedCapability,DomainError> PluginRuntime::loadCapability(const CapabilityRef& ref,const ContextSpec& context) { return loadForDispatch(ref,context,{}); }
Result<LoadedCapability,DomainError> PluginRuntime::loadForDispatch(const CapabilityRef& ref,const ContextSpec& context,const std::shared_ptr<ToolDispatchToken>& token) {
    { QMutexLocker lock(&m_state->mutex); if (m_state->stopping || (token && !token->allowed())) return fail<LoadedCapability>("CANCELLED","Capability load cancelled"); }
    if (context.profileId!=m_profileId) return fail<LoadedCapability>("INVALID_INPUT","Capability context has the wrong profile");
    if (ref.pluginId=="builtin.learned") { QMutexLocker lock(&m_state->mutex); return LearnedSkillAdapter::load(m_state->learned,ref,context); }
    if (context.originKind!=OriginKind::LocalOwner) { auto allowed=grant(ref,context); if (!allowed) return Result<LoadedCapability,DomainError>::failure(allowed.error()); }
    std::shared_ptr<PluginCatalog> catalog; { QMutexLocker lock(&m_state->mutex); catalog=m_state->catalog; }
    auto loaded=catalog->loadCapability(ref); if (!loaded) return loaded;
    loaded.value().hostConfiguration=binding(ref).configuration;
    if (!ref.operationId.isEmpty() && loaded.value().operation["parameters"].toObject()["source"]=="mcp") {
        auto allowed=grant(ref,context); if (!allowed) return Result<LoadedCapability,DomainError>::failure(allowed.error());
        CapabilityCall probe; probe.context=context; probe.ref=ref;
        auto bound=binding(ref); auto permitted=authorize(loaded.value(),probe,allowed.value(),bound);
        if (!permitted) return Result<LoadedCapability,DomainError>::failure(permitted.error());
        { QMutexLocker lock(&m_state->mutex);
            if (m_state->stopping || loaded.value().catalogRevision!=m_state->catalog->revision() || (token && !token->allowed())) return fail<LoadedCapability>("CANCELLED","Capability load invalidated before connection");
        }
        auto schema=CapabilityExecutor::remoteSchema(loaded.value(),bound,5000);
        if (!schema) return Result<LoadedCapability,DomainError>::failure(schema.error()); loaded.value().parameterSchema=schema.value();
    }
    { QMutexLocker lock(&m_state->mutex); m_state->loaded.insert(context.cacheKey()+ref.key(),loaded.value()); }
    return loaded;
}
Result<void,DomainError> PluginRuntime::authorize(const LoadedCapability& loaded,const CapabilityCall& call,const QJsonObject& permission,const CapabilityBinding& bound) const {
    if (call.autonomous && !permission["autonomous"].toBool()) return fail<void>("PERMISSION_DENIED","Autonomous capability use is not granted");
    const auto effect=loaded.operation["effects"].toObject()["kind"].toString();
    if (!permission["effects"].toArray().contains(effect)) return fail<void>("PERMISSION_DENIED","Capability effect is not granted");
    const auto invocation=loaded.operation["invocation"].toObject();
    const auto executor=bound.configuration[invocation[invocation["kind"]=="command"?"program":"connection"].toObject()["setting"].toString()].toString();
    if (executor.isEmpty() || executor!=permission["executor"].toString()) return fail<void>("PERMISSION_DENIED","Executor does not match the host grant");
    const auto result=loaded.operation["result"].toObject();
    const auto adapter=bound.resultAdapter;
    if (result["completion"]=="reported" && (!adapter["statePointer"].isString() || !adapter["stateMap"].isObject())) return fail<void>("DEPENDENCY_UNAVAILABLE","Reported result adapter is missing");
    if ((result["completion"]=="accepted" || ([&]{ const auto map=adapter["stateMap"].toObject(); for (const auto& v:map) if (v=="accepted") return true; return false; }()))
        && (!adapter["receiptPointer"].isString() || !adapter["trackingPointer"].isString())) return fail<void>("DEPENDENCY_UNAVAILABLE","Accepted result tracking adapter is missing");
    // Schema loading checks executor binding without requiring business arguments.
    if (call.callId.isEmpty()) return Result<void,DomainError>::success();
    const auto objects=permission["objectArguments"].toObject();
    for (auto it=objects.begin();it!=objects.end();++it) { auto value=CapabilityExecutor::pointer(call.arguments,it.key()); if (!value || value.value()!=it.value()) return fail<void>("PERMISSION_DENIED","Capability object is outside the granted scope"); }
    for (const auto& item:permission["pathArguments"].toArray()) {
        auto value=CapabilityExecutor::pointer(call.arguments,item.toString());
        if (!value || !value.value().isString() || !QDir::isAbsolutePath(value.value().toString())) return fail<void>("PERMISSION_DENIED","Capability path is not granted");
        const auto path=canonicalPath(value.value().toString()); bool inside=false;
        for (const auto& root:permission["allowedRootPaths"].toArray()) { const auto canonical=canonicalPath(root.toString()); if (!canonical.isEmpty() && (path==canonical || path.startsWith(canonical+'/'))) inside=true; }
        if (!inside) return fail<void>("PERMISSION_DENIED","Capability path is outside granted roots");
    }
    return Result<void,DomainError>::success();
}
void PluginRuntime::settle(const std::shared_ptr<Pending>& pending) {
    if (pending->settled || !pending->outcome) return;
    pending->settled=true; m_pending.removeAll(pending);
    emit capabilityFinished(pending->outcome->toJson());
    if (pending->completion) pending->completion(std::move(*pending->outcome));
}
void PluginRuntime::invokeCapability(const CapabilityCall& call,CapabilityCompletion completion) {
    auto pending=std::make_shared<Pending>(); pending->completion=std::move(completion); m_pending.append(pending);
    QPointer<PluginRuntime> guard(this);
    m_worker.start([this,guard,pending,call] {
        std::optional<LoadedCapability> executedDefinition;
        auto execute=[&]() -> CapabilityOutcome {
            if (call.callId.isEmpty() || call.activityId.isEmpty() || call.context.profileId!=m_profileId || call.ref.operationId.isEmpty()) return errorOutcome(call,domainError("INVALID_INPUT","Incomplete host capability call"));
            std::shared_ptr<PluginCatalog> catalog; { QMutexLocker lock(&m_state->mutex); catalog=m_state->catalog; }
            if (!catalog->enabled(call.ref.pluginId)) return errorOutcome(call,domainError("PLUGIN_DISABLED","Plugin is disabled"));
            if (call.catalogRevision!=catalog->revision()) return errorOutcome(call,domainError("STALE_REVISION","Loaded capability is stale"));
            auto loaded=catalog->loadCapability(call.ref); if (!loaded) return errorOutcome(call,loaded.error());
            if (loaded.value().definitionDigest!=call.definitionDigest) return errorOutcome(call,domainError("STALE_REVISION","Loaded definition changed"));
            auto permission=grant(call.ref,call.context,call.grantRef); if (!permission) return errorOutcome(call,permission.error());
            executedDefinition=loaded.value();
            const auto bound=binding(call.ref); executedDefinition->hostConfiguration=bound.configuration; auto authorized=authorize(loaded.value(),call,permission.value(),bound); if (!authorized) return errorOutcome(call,authorized.error());
            auto recheck=[&,catalog](bool start) -> Result<void,DomainError> {
                // No lock survives process/network I/O. This check is immediately before dispatch.
                { QMutexLocker lock(&m_state->mutex);
                    if (m_state->stopping) return fail<void>("CANCELLED","Capability runtime is closing");
                    if (!m_state->catalog->enabled(call.ref.pluginId)) return fail<void>("PLUGIN_DISABLED","Plugin is disabled");
                    if (m_state->catalog!=catalog || call.catalogRevision!=catalog->revision()) return fail<void>("STALE_REVISION","Capability configuration changed");
                }
                auto current=catalog->loadCapability(call.ref); if (!current) return Result<void,DomainError>::failure(current.error());
                auto currentGrant=grant(call.ref,call.context,call.grantRef); if (!currentGrant) return Result<void,DomainError>::failure(currentGrant.error());
                auto allowed=authorize(current.value(),call,currentGrant.value(),binding(call.ref));
                if (allowed) {
                    QMutexLocker lock(&m_state->mutex);
                    if (m_state->stopping || m_state->catalog!=catalog || call.catalogRevision!=catalog->revision() || !catalog->enabled(call.ref.pluginId)) return fail<void>("STALE_REVISION","Capability binding changed before dispatch");
                    if (call.dispatchToken && !(start?call.dispatchToken->begin():call.dispatchToken->allowed())) return fail<void>("CANCELLED","Capability cancelled before dispatch");
                }
                if (allowed && start) emit capabilityDispatching(call.callId);
                return allowed;
            };
            return CapabilityExecutor::execute(loaded.value(),call,bound,recheck);
        };
        try { pending->outcome=execute(); }
        catch (...) { auto out=errorOutcome(call,domainError("ACTION_OUTCOME_UNKNOWN","Capability execution raised an exception")); out.stage="unknown"; out.effectCertainty="unknown"; pending->outcome=out; }
        if (executedDefinition) {
            pending->outcome->data=publicValue(pending->outcome->data,*executedDefinition);
            pending->outcome->receipt=publicValue(pending->outcome->receipt,*executedDefinition);
            pending->outcome->tracking=publicValue(pending->outcome->tracking,*executedDefinition);
        }
        pending->outcome->context=call.context; pending->outcome->catalogRevision=call.catalogRevision;
        if (guard) QMetaObject::invokeMethod(guard,[guard,pending]{ if (guard) guard->settle(pending); },Qt::QueuedConnection);
    });
}
Result<void,DomainError> PluginRuntime::setPluginEnabled(const PluginStateChange& change) {
    QMutexLocker lock(&m_state->mutex);
    if (change.expectedRevision!=m_state->catalog->revision()) return fail<void>("STALE_REVISION","Catalog revision changed");
    auto config=m_state->config; auto plugins=config["plugins"].toArray(); bool found=false;
    for (int i=0;i<plugins.size();++i) {
        auto value=plugins[i].toObject();
        if (value["pluginId"]==change.pluginId) { value["enabled"]=change.enabled; plugins[i]=value; found=true; }
    }
    if (!found) return fail<void>("UNKNOWN_REFERENCE","Plugin is not in host settings");
    config["plugins"]=plugins; auto saved=save(config); if (!saved) return saved;
    auto changed=m_state->catalog->setPluginEnabled(change); if (changed) m_state->config=config;
    return changed;
}

void PluginRuntime::retire() {
    { QMutexLocker lock(&m_state->mutex); m_state->stopping=true; }
    auto* timer=new QTimer(this); timer->setInterval(25);
    connect(timer,&QTimer::timeout,this,[this,timer]{
        if (m_worker.activeThreadCount()==0 && m_pending.isEmpty()) { timer->stop(); deleteLater(); }
    }); timer->start();
}

QJsonValue PluginRuntime::publicValue(const QJsonValue& value,const LoadedCapability& loaded) const {
    const auto configuration=loaded.hostConfiguration; QStringList secrets;
    for (const auto& item:loaded.configuration) {
        const auto declared=item.toObject(); const auto secret=configuration[declared["key"].toString()].toString();
        if (declared["kind"]=="secret" && !secret.isEmpty()) secrets.append(secret);
    }
    std::function<QJsonValue(const QJsonValue&)> redact=[&](const QJsonValue& v)->QJsonValue {
        if (v.isString()) { auto text=v.toString(); for (const auto& secret:secrets) text.replace(secret,"[REDACTED]"); return text; }
        if (v.isArray()) { QJsonArray a; for (const auto& child:v.toArray()) a.append(redact(child)); return a; }
        if (v.isObject()) { const auto original=v.toObject(); QJsonObject o; for (auto it=original.begin();it!=original.end();++it) o[redact(it.key()).toString()]=redact(it.value()); return o; }
        return v;
    };
    return redact(value);
}
