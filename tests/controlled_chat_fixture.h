#pragma once
#include "ai/event/runtime_unit_of_work.h"
#include "ai/plugin/plugin_runtime.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QSqlQuery>
#include <QTemporaryDir>
namespace ControlledChat {
inline const QString profile = "5bb00e6d-937a-4f46-9c87-e3933c078f5a";
inline QJsonObject readJson(const QString &path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
    return {};
  return QJsonDocument::fromJson(f.readAll()).object();
}
inline bool writeJson(const QString &path, const QJsonObject &value) {
  QFile f(path);
  return f.open(QIODevice::WriteOnly) &&
         f.write(QJsonDocument(value).toJson()) > 0;
}
struct Fixture {
  QTemporaryDir directory;
  QString root = directory.filePath("package"),
          path = directory.filePath("runtime.sqlite"),
          journal = directory.filePath("journal.sqlite");
  QString plugin = "example.controlled-chat-plugin",
          entry = "example.controlled-chat",
          source = "example.controlled-events", read = "read", send = "send";
  QJsonObject configuration;
  bool valid = false;
  Fixture(bool renamed = false, bool withSource = true) {
    QDir().mkpath(root);
    for (const auto &name :
         {"plugin.json", "tool.json", "source.json", "USAGE.md", "adapter.py"})
      if (!QFile::copy(QString(CONTROLLED_CHAT_PATH) + "/" + name,
                       root + "/" + name))
        return;
    if (renamed) {
      plugin = "another.messenger";
      entry = "another.channel";
      source = "another.stream";
      read = "inspect_page";
      send = "publish_note";
      auto t = readJson(root + "/tool.json");
      t["id"] = entry;
      auto d = t["definition"].toObject();
      auto ops = d["operations"].toArray();
      for (int i = 0; i < ops.size(); ++i) {
        auto op = ops[i].toObject();
        if (op["id"] == "read")
          op["id"] = read;
        if (op["id"] == "send")
          op["id"] = send;
        ops[i] = op;
      }
      d["operations"] = ops;
      t["definition"] = d;
      writeJson(root + "/tool.json", t);
      auto s = readJson(root + "/source.json");
      s["id"] = source;
      writeJson(root + "/source.json", s);
      auto p = readJson(root + "/plugin.json");
      p["id"] = plugin;
      p["entries"] = QJsonArray{QJsonObject{
          {"kind", "tool"}, {"id", entry}, {"definition", "tool.json"}}};
      p["sources"] = QJsonArray{QJsonObject{{"id", source},
                                            {"summary", "Optional stream"},
                                            {"definition", "source.json"}}};
      writeJson(root + "/plugin.json", p);
    }
    if (!withSource) {
      auto p = readJson(root + "/plugin.json");
      p.remove("sources");
      writeJson(root + "/plugin.json", p);
    }
    QJsonObject settings{{"program", "/usr/bin/python3"},
                         {"script", root + "/adapter.py"},
                         {"database", journal}};
    QJsonObject observation{
        {"id", "account"},
        {"pluginId", plugin},
        {"channel", "controlled-chat"},
        {"connectionKey", "account-one"},
        {"provider", "controlled-chat"},
        {"namespace", "account-one"},
        {"selfIdentity", QJsonObject{{"provider", "controlled-chat"},
                                     {"namespace", "account-one"},
                                     {"remoteActorId", "pet-account"}}},
        {"conversations",
         QJsonArray{QJsonObject{{"remoteConversationId", "group-a"},
                                {"kind", "group"}},
                    QJsonObject{{"remoteConversationId", "group-b"},
                                {"kind", "group"}}}}};
    if (withSource)
      observation["source"] = QJsonObject{{"sourceId", source},
                                          {"configurationKey", source},
                                          {"executor", "/usr/bin/python3"},
                                          {"enabled", false}};
    QJsonArray grants;
    for (const auto &scope : {QString(),
                              conversationIdFor(profile, "controlled-chat",
                                                "account-one", "group-a"),
                              conversationIdFor(profile, "controlled-chat",
                                                "account-one", "group-b")})
      for (const auto &op : {read, send, QString("list"), QString("receipt")}) {
        if (scope.isEmpty() && op == send)
          continue;
        QJsonObject g{{"id", scope + ":" + op},
                      {"profileId", profile},
                      {"pluginId", plugin},
                      {"entryId", entry},
                      {"operationId", op},
                      {"effects", QJsonArray{op == send ? "write" : "read"}},
                      {"executor", "/usr/bin/python3"},
                      {"autonomous", true}};
        if (scope.isEmpty())
          g["scope"] = "internal_activity";
        else
          g["conversationId"] = scope;
        grants.append(g);
      }
    QJsonArray replies;
    for (const auto &room : {"group-a", "group-b"})
      replies.append(QJsonObject{
          {"channel", "controlled-chat"},
          {"connectionKey", "account-one"},
          {"remoteConversationId", room},
          {"kind", "group"},
          {"selfIdentity", observation["selfIdentity"]},
          {"ref", ref(send).toJson()},
          {"grantRef",
           conversationIdFor(profile, "controlled-chat", "account-one", room) +
               ":" + send},
          {"argumentBindings", QJsonObject{{"/chat_id", "remoteConversationId"},
                                           {"/text", "content"},
                                           {"/reply_to", "replyToRemoteId"}}}});
    configuration = {
        {"plugins",
         QJsonArray{QJsonObject{{"rootPath", root}, {"enabled", true}}}},
        {"configuration", QJsonObject{{entry, settings}, {source, settings}}},
        {"grants", grants},
        {"replyBindings", replies},
        {"observationBindings", QJsonArray{observation}},
        {"observationAdapters",
         QJsonArray{QJsonObject{
             {"ref", ref(read).toJson()},
             {"observationBindingId", "account"},
             {"itemsPointer", "/items"},
             {"fields", QJsonObject{{"eventId", "/event_id"},
                                    {"sequence", "/sequence"},
                                    {"cursor", "/cursor"},
                                    {"remoteConversationId", "/chat_id"},
                                    {"remoteMessageId", "/message_id"},
                                    {"text", "/text"},
                                    {"remoteActorId", "/speaker_id"},
                                    {"occurredAt", "/occurred_at"},
                                    {"replyToRemoteId", "/reply_to"}}}}}}};
    ConversationStore store(profile, path);
    valid = bool(store.initialize());
  }
  bool sourceProbe(const QJsonArray &events, const QString &mode) {
    const auto wire = directory.filePath("wire.json"),
               log = directory.filePath("source-log.jsonl");
    QFile data(wire);
    if (!data.open(QIODevice::WriteOnly))
      return false;
    data.write(QJsonDocument(events).toJson());
    data.close();
    auto definition = readJson(root + "/source.json");
    auto declarations = definition["configuration"].toArray();
    for (const auto &key : {"wire", "log"})
      declarations.append(
          QJsonObject{{"key", key},
                      {"kind", "path"},
                      {"required", true},
                      {"description", "Controlled protocol fixture"}});
    definition["configuration"] = declarations;
    definition["invocation"] = QJsonObject{
        {"kind", "command"},
        {"program", QJsonObject{{"setting", "program"}}},
        {"arguments", QJsonArray{QJsonObject{{"setting", "script"}},
                                 QJsonObject{{"setting", "wire"}},
                                 QJsonObject{{"setting", "log"}}, mode}}};
    if (!writeJson(root + "/source.json", definition))
      return false;
    auto settings = configuration["configuration"].toObject();
    settings[source] =
        QJsonObject{{"program", "/usr/bin/python3"},
                    {"script", QString(OBSERVATION_SOURCE_PROBE)},
                    {"database", journal},
                    {"wire", wire},
                    {"log", log}};
    configuration["configuration"] = settings;
    auto bindings = configuration["observationBindings"].toArray();
    auto b = bindings[0].toObject();
    auto control = b["source"].toObject();
    control["enabled"] = true;
    b["source"] = control;
    bindings[0] = b;
    configuration["observationBindings"] = bindings;
    return true;
  }
  CapabilityRef ref(const QString &operation) const {
    return {plugin, entry, "1.0.0", operation};
  }
  ObservationBinding binding() const {
    auto c = ObservationConfiguration::parse(configuration);
    return c ? c.value().bindings["account"] : ObservationBinding{};
  }
  QJsonObject inject(QString actor = "alice",
                     QString text = "A new real message",
                     QString room = "group-a", QString reply = {}) {
    QProcess process;
    process.start("/usr/bin/python3", {root + "/adapter.py", "--database",
                                       journal, "--operation", "inject"});
    if (!process.waitForStarted())
      return {};
    process.write(QJsonDocument(QJsonObject{{"chat_id", room},
                                            {"speaker_id", actor},
                                            {"text", text},
                                            {"reply_to", reply}})
                      .toJson());
    process.closeWriteChannel();
    if (!process.waitForFinished(5000) || process.exitCode() != 0)
      return {};
    return QJsonDocument::fromJson(process.readAllStandardOutput()).object();
  }
  ObservationEnvelope envelope(QString seq = "1", QString room = "group-a",
                               QString id = "message-1",
                               QString actor = "alice") const {
    return {
        "account",
        QJsonObject{{"type", "observation"},
                    {"kind", "message"},
                    {"eventId", "event-" + seq},
                    {"sequence", seq},
                    {"cursor", "cursor:" + seq},
                    {"conversation", QJsonObject{{"remoteConversationId", room},
                                                 {"kind", "group"}}},
                    {"speaker", QJsonObject{{"provider", "controlled-chat"},
                                            {"namespace", "account-one"},
                                            {"remoteActorId", actor}}},
                    {"message", QJsonObject{{"remoteMessageId", id},
                                            {"text", "An observed message"}}}},
        1};
  }
  bool sql(const QString &statement) const {
    auto u = SqliteRuntimeUnitOfWork::begin(path);
    if (!u)
      return false;
    QSqlQuery q(QSqlDatabase::database(u.value()->connectionName()));
    return q.exec(statement) && u.value()->commit();
  }
  int count(const QString &table, const QString &where = {}) const {
    auto u = SqliteRuntimeUnitOfWork::begin(path);
    if (!u)
      return -1;
    QSqlQuery q(QSqlDatabase::database(u.value()->connectionName()));
    if (!q.exec("SELECT count(*) FROM " + table +
                (where.isEmpty() ? QString() : " WHERE " + where)) ||
        !q.next())
      return -1;
    return q.value(0).toInt();
  }
};
} // namespace ControlledChat
