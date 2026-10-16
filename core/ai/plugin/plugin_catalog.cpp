#include "plugin_catalog.h"
#include "schema_validator.h"
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSet>
#include <algorithm>

namespace {
template<class T> Result<T,DomainError> fail(const QString& code,const QString& message) { return Result<T,DomainError>::failure(domainError(code,message)); }
Result<QJsonObject,DomainError> readObject(const QString& path) {
    QFile file(path); if (!file.open(QIODevice::ReadOnly) || file.size()>2*1024*1024) return fail<QJsonObject>("DEPENDENCY_UNAVAILABLE","Manifest unavailable");
    QJsonParseError error; const auto doc=QJsonDocument::fromJson(file.readAll(),&error);
    if (error.error!=QJsonParseError::NoError || !doc.isObject()) return fail<QJsonObject>("INVALID_INPUT","Manifest must be a JSON object");
    return Result<QJsonObject,DomainError>::success(doc.object());
}
}
Result<QString,DomainError> PluginCatalog::digestPackage(const QString& root) {
    QStringList paths; QDirIterator files(root,QDir::Files|QDir::Hidden,QDirIterator::Subdirectories);
    while (files.hasNext()) { paths.append(files.next()); if (paths.size()>1024) return fail<QString>("INVALID_INPUT","Package exceeds file limit"); }
    paths.sort(); QCryptographicHash hash(QCryptographicHash::Sha256); qint64 total=0;
    for (const auto& path:paths) {
        const QString relative=QDir(root).relativeFilePath(path);
        const auto resolved=SchemaValidator::packagePath(root,relative);
        if (!resolved) return Result<QString,DomainError>::failure(resolved.error());
        QFile file(resolved.value()); if (!file.open(QIODevice::ReadOnly) || (total+=file.size())>16*1024*1024) return fail<QString>("DEPENDENCY_UNAVAILABLE","Package content unavailable or too large");
        hash.addData(relative.toUtf8()); hash.addData(QByteArray(1,'\0')); hash.addData(file.readAll());
    }
    return Result<QString,DomainError>::success(QString::fromLatin1(hash.result().toHex()));
}
Result<PluginRegistration,DomainError> PluginCatalog::registerPlugin(const PluginLocation& location) {
    const QString root=QFileInfo(location.rootPath).canonicalFilePath();
    auto manifestPath=SchemaValidator::packagePath(root,"plugin.json");
    if (!manifestPath) return Result<PluginRegistration,DomainError>::failure(manifestPath.error());
    auto manifest=readObject(manifestPath.value()); if (!manifest) return Result<PluginRegistration,DomainError>::failure(manifest.error());
    auto valid=SchemaValidator::validateManifest("plugin",manifest.value()); if (!valid) return Result<PluginRegistration,DomainError>::failure(valid.error());
    const QString pluginId=manifest.value()["id"].toString();
    QHash<QString,Entry> staged;
    for (const auto& item:manifest.value()["entries"].toArray()) {
        const auto descriptor=item.toObject(); const auto kind=descriptor["kind"].toString(), id=descriptor["id"].toString();
        if (staged.contains(id)) return fail<PluginRegistration>("IDENTITY_CONFLICT","Duplicate entry identity");
        auto path=SchemaValidator::packagePath(root,descriptor["definition"].toString());
        if (!path) return Result<PluginRegistration,DomainError>::failure(path.error());
        auto content=readObject(path.value()); if (!content) return Result<PluginRegistration,DomainError>::failure(content.error());
        valid=SchemaValidator::validateManifest(kind=="tool"?"capability":"skill",content.value());
        if (!valid) return Result<PluginRegistration,DomainError>::failure(valid.error());
        if (content.value()["id"].toString()!=id) return fail<PluginRegistration>("INVALID_INPUT","Entry identity disagrees with manifest");
        const auto definition=kind=="tool"?content.value()["definition"].toObject():content.value();
        auto instructions=SchemaValidator::packagePath(root,definition["instructions"].toString(),QFileInfo(path.value()).absolutePath());
        if (!instructions) return Result<PluginRegistration,DomainError>::failure(instructions.error());
        Entry entry; entry.path=path.value(); entry.dependencies=definition["dependencies"].toArray();
        entry.summary={{"kind",kind},{"id",id},{"pluginId",pluginId},{"name",content.value()["name"]},
            {"version",content.value()["version"]},{"summary",content.value()["summary"]},
            {"definitionRef",descriptor["definition"]},{"readiness","unchecked"}};
        QSet<QString> requirements;
        for (const auto& requirement:definition["requirements"].toArray()) {
            const auto id=requirement.toObject()["id"].toString();
            if (requirements.contains(id)) return fail<PluginRegistration>("INVALID_INPUT","Duplicate requirement identity");
            requirements.insert(id);
        }
        QHash<QString,QJsonObject> settings;
        for (const auto& config:definition["configuration"].toArray()) {
            const auto object=config.toObject(); const auto key=object["key"].toString();
            if (settings.contains(key)) return fail<PluginRegistration>("INVALID_INPUT","Duplicate configuration key");
            settings.insert(key,object);
        }
        for (const auto& operationValue:definition["operations"].toArray()) {
            const auto operation=operationValue.toObject(); const auto opId=operation["id"].toString();
            if (entry.operationIds.contains(opId)) return fail<PluginRegistration>("INVALID_INPUT","Duplicate operation identity");
            entry.operationIds.append(opId);
            const auto parameters=operation["parameters"].toObject();
            if (parameters["source"]=="inline") {
                valid=SchemaValidator::check(parameters["schema"],root,path.value());
                if (!valid) return Result<PluginRegistration,DomainError>::failure(valid.error());
            }
            const auto result=operation["result"].toObject();
            if (result.contains("schema")) { valid=SchemaValidator::check(result["schema"],root,path.value()); if (!valid) return Result<PluginRegistration,DomainError>::failure(valid.error()); }
            const auto invocation=operation["invocation"].toObject();
            auto references=invocation["arguments"].toArray(); references.append(invocation["program"]); references.append(invocation["connection"]); references.append(invocation["workingDirectory"]);
            for (const auto& reference:references) if (reference.isObject() && reference.toObject().contains("setting")) {
                const auto setting=settings.value(reference.toObject()["setting"].toString());
                if (setting.isEmpty() || !setting["required"].toBool()) return fail<PluginRegistration>("INVALID_INPUT","Invocation references undeclared or optional configuration");
            }
            if (invocation.contains("workingDirectory") && settings.value(invocation["workingDirectory"].toObject()["setting"].toString())["kind"]!="path")
                return fail<PluginRegistration>("INVALID_INPUT","Working directory must use path configuration");
            for (const auto& argument:invocation["arguments"].toArray()) {
                const auto key=argument.toObject()["setting"].toString();
                if (!key.isEmpty() && settings.value(key)["kind"]=="mcp_connection") return fail<PluginRegistration>("INVALID_INPUT","Command argument cannot bind an MCP connection");
            }
            if (invocation["kind"]=="command" && settings.value(invocation["program"].toObject()["setting"].toString())["kind"]!="executable")
                return fail<PluginRegistration>("INVALID_INPUT","Program binding requires executable configuration");
            if (invocation["kind"]=="mcp" && settings.value(invocation["connection"].toObject()["setting"].toString())["kind"]!="mcp_connection")
                return fail<PluginRegistration>("INVALID_INPUT","MCP binding requires connection configuration");
        }
        staged.insert(id,entry);
    }
    QHash<QString, QString> sources;
    for (const auto& source:manifest.value()["sources"].toArray()) {
      const auto id = source.toObject()["id"].toString();
      if (sources.contains(id))
        return fail<PluginRegistration>("INVALID_INPUT",
                                        "Duplicate source identity");
      auto path = SchemaValidator::packagePath(
          root, source.toObject()["definition"].toString());
      if (!path)
        return Result<PluginRegistration, DomainError>::failure(path.error());
      auto definition = readObject(path.value());
      if (!definition)
        return Result<PluginRegistration, DomainError>::failure(
            definition.error());
      auto checked =
          SchemaValidator::validateManifest("source", definition.value());
      if (!checked)
        return Result<PluginRegistration, DomainError>::failure(
            checked.error());
      if (definition.value()["id"] != id)
        return fail<PluginRegistration>(
            "INVALID_INPUT", "Source identity disagrees with manifest");
      QHash<QString, QJsonObject> settings;
      QSet<QString> requirements;
      for (const auto &value : definition.value()["requirements"].toArray()) {
        const auto key = value.toObject()["id"].toString();
        if (requirements.contains(key))
          return fail<PluginRegistration>("INVALID_INPUT",
                                          "Duplicate source requirement");
        requirements.insert(key);
      }
      for (const auto &value : definition.value()["configuration"].toArray()) {
        const auto item = value.toObject();
        const auto key = item["key"].toString();
        if (settings.contains(key) || item["kind"] == "mcp_connection")
          return fail<PluginRegistration>("INVALID_INPUT",
                                          "Invalid source setting");
        settings.insert(key, item);
      }
      const auto invocation = definition.value()["invocation"].toObject();
      auto refs = invocation["arguments"].toArray();
      refs.append(invocation["program"]);
      for (const auto &ref : refs)
        if (ref.isObject()) {
          const auto setting =
              settings.value(ref.toObject()["setting"].toString());
          if (setting.isEmpty() || !setting["required"].toBool())
            return fail<PluginRegistration>(
                "INVALID_INPUT",
                "Source references undeclared or optional setting");
        }
      if (settings.value(
              invocation["program"].toObject()["setting"].toString())["kind"] !=
          "executable")
        return fail<PluginRegistration>("INVALID_INPUT",
                                        "Source requires executable setting");
      sources.insert(id, path.value());
    }
    auto digest=digestPackage(root); if (!digest) return Result<PluginRegistration,DomainError>::failure(digest.error());
    QMutexLocker lock(&m_mutex);
    for (auto it=staged.cbegin();it!=staged.cend();++it)
        if (m_entries.contains(it.key()) && m_entries[it.key()].summary["pluginId"]!=pluginId)
            return fail<PluginRegistration>("IDENTITY_CONFLICT","Entry already belongs to another plugin");
    bool isEnabled=true;
    if (m_packages.contains(pluginId)) {
        const auto old=m_packages.value(pluginId); isEnabled=old.enabled;
        for (const auto& id:old.entryIds) m_entries.remove(id);
    }
    Package package{root, digest.value(), staged.keys(), isEnabled, sources};
    m_packages.insert(pluginId,package);
    PluginRegistration registration; registration.pluginId=pluginId; registration.catalogRevision=++m_revision;
    for (auto it=staged.cbegin();it!=staged.cend();++it) { m_entries.insert(it.key(),it.value()); registration.entries.append(it.value().summary); }
    return Result<PluginRegistration,DomainError>::success(registration);
}
Result<LoadedCapability,DomainError> PluginCatalog::loadCapability(const CapabilityRef& ref) const {
    Entry entry; Package package; quint64 revision;
    {
        QMutexLocker lock(&m_mutex);
        if (!m_entries.contains(ref.entryId) || m_entries[ref.entryId].summary["pluginId"]!=ref.pluginId) return fail<LoadedCapability>("UNKNOWN_REFERENCE","Capability identity is not registered");
        entry=m_entries.value(ref.entryId); package=m_packages.value(ref.pluginId); revision=m_revision;
        if (!package.enabled) return fail<LoadedCapability>("PLUGIN_DISABLED","Plugin is disabled");
        if (entry.summary["version"].toString()!=ref.entryVersion) return fail<LoadedCapability>("STALE_REVISION","Capability version changed");
        for (const auto& dependency:entry.dependencies) {
            const auto d=dependency.toObject(); const auto found=m_entries.constFind(d["id"].toString());
            bool ready=found!=m_entries.cend() && found->summary["kind"]=="tool" && found->summary["version"]==d["version"]
                && m_packages.value(found->summary["pluginId"].toString()).enabled;
            if (ready) for (const auto& op:d["operations"].toArray()) ready &= found->operationIds.contains(op.toString());
            if (!ready && d["required"].toBool()) return fail<LoadedCapability>("DEPENDENCY_UNAVAILABLE","Required capability dependency is unavailable or incompatible");
        }
    }
    auto digest=digestPackage(package.root); if (!digest) return Result<LoadedCapability,DomainError>::failure(digest.error());
    if (digest.value()!=package.digest) return fail<LoadedCapability>("STALE_REVISION","Package content changed; register it again");
    auto manifest=readObject(entry.path); if (!manifest) return Result<LoadedCapability,DomainError>::failure(manifest.error());
    LoadedCapability loaded; loaded.ref=ref; loaded.kind=entry.summary["kind"].toString(); loaded.catalogRevision=revision;
    loaded.definitionDigest=package.digest; loaded.rootPath=package.root; loaded.definitionPath=entry.path;
    const auto definition=loaded.kind=="tool"?manifest.value()["definition"].toObject():manifest.value();
    auto path=SchemaValidator::packagePath(package.root,definition["instructions"].toString(),QFileInfo(entry.path).absolutePath());
    if (!path) return Result<LoadedCapability,DomainError>::failure(path.error());
    QFile instructions(path.value()); if (!instructions.open(QIODevice::ReadOnly) || instructions.size()>2*1024*1024) return fail<LoadedCapability>("DEPENDENCY_UNAVAILABLE","Instructions unavailable");
    loaded.instructions=QString::fromUtf8(instructions.readAll()); loaded.dependencies=entry.dependencies; loaded.configuration=definition["configuration"].toArray();
    for (const auto& value:definition["operations"].toArray()) {
        const auto operation=value.toObject(); loaded.operations.append(QJsonObject{{"id",operation["id"]},{"description",operation["description"]}});
        if (operation["id"]==ref.operationId) { loaded.operation=operation; loaded.parameterSchema=operation["parameters"].toObject()["schema"].toObject(); }
    }
    if ((!ref.operationId.isEmpty() && loaded.operation.isEmpty()) || (loaded.kind=="skill" && !ref.operationId.isEmpty())) return fail<LoadedCapability>("UNKNOWN_REFERENCE","Operation does not exist");
    return Result<LoadedCapability,DomainError>::success(loaded);
}
Result<void,DomainError> PluginCatalog::setPluginEnabled(const PluginStateChange& change) {
    QMutexLocker lock(&m_mutex);
    if (change.expectedRevision!=m_revision) return fail<void>("STALE_REVISION","Catalog revision changed");
    if (!m_packages.contains(change.pluginId)) return fail<void>("UNKNOWN_REFERENCE","Plugin is not registered");
    m_packages[change.pluginId].enabled=change.enabled; ++m_revision;
    return Result<void,DomainError>::success();
}
QJsonArray PluginCatalog::discover() const {
    QMutexLocker lock(&m_mutex); QJsonArray result; auto keys=m_entries.keys(); keys.sort();
    for (const auto& key:keys) { auto entry=m_entries[key].summary; if (!m_packages.value(entry["pluginId"].toString()).enabled) entry["readiness"]="disabled"; result.append(entry); }
    return result;
}
quint64 PluginCatalog::revision() const { QMutexLocker lock(&m_mutex); return m_revision; }
void PluginCatalog::invalidate() { QMutexLocker lock(&m_mutex); ++m_revision; }
bool PluginCatalog::enabled(const QString& id) const { QMutexLocker lock(&m_mutex); return m_packages.contains(id) && m_packages[id].enabled; }

Result<LoadedObservationSource, DomainError>
PluginCatalog::loadSource(const QString &pluginId,
                          const QString &sourceId) const {
  using R = Result<LoadedObservationSource, DomainError>;
  Package package;
  quint64 revision;
  {
    QMutexLocker lock(&m_mutex);
    if (!m_packages.contains(pluginId) ||
        !m_packages[pluginId].sources.contains(sourceId))
      return fail<LoadedObservationSource>(
          "UNKNOWN_REFERENCE", "Source is not declared by this plugin");
    package = m_packages[pluginId];
    revision = m_revision;
    if (!package.enabled)
      return fail<LoadedObservationSource>("PLUGIN_DISABLED",
                                           "Source plugin is disabled");
  }
  auto digest = digestPackage(package.root);
  if (!digest)
    return R::failure(digest.error());
  if (digest.value() != package.digest)
    return fail<LoadedObservationSource>("STALE_REVISION",
                                         "Source package content changed");
  auto definition = readObject(package.sources[sourceId]);
  if (!definition)
    return R::failure(definition.error());
  return R::success({pluginId, sourceId, package.root, package.digest, revision,
                     definition.value()});
}
