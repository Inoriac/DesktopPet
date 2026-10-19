#include "python_test_support.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include "ai/plugin/plugin_catalog.h"
#include "ai/plugin/schema_validator.h"
#include "ai/plugin/plugin_runtime.h"
#include <QDir>
#include <QSignalSpy>

namespace {
void writeJson(const QString& path, const QJsonObject& object) {
    QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(object).toJson());
}
void makeSkill(const QString& root) {
    writeJson(root + "/plugin.json", {{"formatVersion", "1"}, {"id", "example.plugin"},
        {"name", "Plugin"}, {"version", "1"}, {"summary", "A method"},
        {"entries", QJsonArray{QJsonObject{{"kind", "skill"}, {"id", "example.method"}, {"definition", "skill.json"}}}}});
    writeJson(root + "/skill.json", {{"formatVersion", "1"}, {"id", "example.method"},
        {"name", "Method"}, {"version", "1"}, {"summary", "Summarize supplied material"},
        {"instructions", "SKILL.md"}, {"dependencies", QJsonArray{}}});
    QFile file(root + "/SKILL.md"); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("Use supplied facts. PRIVATE_METHOD_BODY");
}
void makeTool(const QString& root, const QString& kind = "command") {
    QDir().mkpath(root);
    QJsonObject invocation = kind == "command"
        ? QJsonObject{{"kind","command"},{"program",QJsonObject{{"setting","program"}}},
            {"arguments",QJsonArray{QJsonObject{{"setting","script"}}}},{"stdin","json"}}
        : QJsonObject{{"kind","mcp"},{"connection",QJsonObject{{"setting","connection"}}},{"tool","probe"}};
    QJsonArray config = kind == "command" ? QJsonArray{
        QJsonObject{{"key","program"},{"kind","executable"},{"required",true},{"description","Program"}},
        QJsonObject{{"key","script"},{"kind","path"},{"required",true},{"description","Script"}}}
        : QJsonArray{QJsonObject{{"key","connection"},{"kind","mcp_connection"},{"required",true},{"description","Connection"}}};
    QJsonObject parameters = kind == "command" ? QJsonObject{{"source","inline"},{"schema",QJsonObject{{"type","object"}}}}
        : QJsonObject{{"source","mcp"}};
    writeJson(root + "/tool.json", {{"formatVersion","1"},{"id","example.tool"},{"name","Controlled operation"},{"version","1"},{"summary","Inspect controlled input"},
        {"definition",QJsonObject{{"instructions","USAGE.md"},{"requirements",QJsonArray{}},{"configuration",config},
            {"operations",QJsonArray{QJsonObject{{"id","probe"},{"description","Return controlled result"},{"parameters",parameters},{"invocation",invocation},
                {"effects",QJsonObject{{"kind","read"},{"description","Only test input"}}},
                {"result",QJsonObject{{"format",kind=="command"?"json":"mcp"},{"description","Whole result"},{"successMeaning","Query returned"},{"failureMeaning","Query failed"},{"completion","completed"}}}}}}}}});
    writeJson(root + "/plugin.json",{{"formatVersion","1"},{"id","example.plugin"},{"name","Controlled plugin"},{"version","1"},{"summary","Test package"},
        {"entries",QJsonArray{QJsonObject{{"kind","tool"},{"id","example.tool"},{"definition","tool.json"}}}}});
    QFile f(root + "/USAGE.md"); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("Use probe with supplied input.");
}
QJsonObject hostConfig(const QString& root, const QString& kind = "command") {
    const QString script=QStringLiteral(PLUGIN_FIXTURE_PATH);
    const QJsonObject grant{{"id","test-grant"},{"pluginId","example.plugin"},{"profileId","pet"},{"conversationId","local_owner"},{"entryId","example.tool"},{"operationId","probe"},
        {"effects",QJsonArray{"read"}},{"executor",kind=="command"?testPythonExecutable():QStringLiteral("fixture")},{"autonomous",false}};
    return {{"plugins",QJsonArray{QJsonObject{{"rootPath",root},{"enabled",true}}}},
        {"configuration",QJsonObject{{"example.tool",kind=="command"?QJsonObject{{"program",testPythonExecutable()},{"script",script}}:QJsonObject{{"connection","fixture"}}}}},
        {"grants",QJsonArray{grant}},
        {"connections",QJsonObject{{"fixture",QJsonObject{{"program",testPythonExecutable()},{"arguments",QJsonArray{script,"--mcp"}}}}}}};
}
void editTool(const QString& root,const std::function<void(QJsonObject&)>& edit) {
    QFile f(root+"/tool.json"); QVERIFY(f.open(QIODevice::ReadOnly)); auto object=QJsonDocument::fromJson(f.readAll()).object(); f.close(); edit(object); writeJson(f.fileName(),object);
}
void resultCompletion(const QString& root,const QString& completion) {
    editTool(root,[&](QJsonObject& object) { auto definition=object["definition"].toObject(); auto operations=definition["operations"].toArray(); auto op=operations[0].toObject();
        auto result=op["result"].toObject(); result["completion"]=completion; if (completion=="reported") result["stateDescription"]="status maps done, queued, bad";
        op["result"]=result; operations[0]=op; definition["operations"]=operations; object["definition"]=definition; });
}
CapabilityCall makeCall(const LoadedCapability& loaded) {
    CapabilityCall c; c.callId="call-1"; c.activityId="activity-1"; c.context=ContextSpec::localOwner("pet");
    c.ref=loaded.ref; c.catalogRevision=loaded.catalogRevision; c.definitionDigest=loaded.definitionDigest; c.grantRef="test-grant"; return c;
}

}
class PluginRuntimeTests : public QObject {
    Q_OBJECT
private slots:
  void metadataGatewayPreservesCompleteDefinitions_data() {
      QTest::addColumn<int>("bodyLength");
      QTest::addColumn<bool>("expectedSuccess");
      QTest::addColumn<QString>("bodyCharacter");
      QTest::newRow("long-complete-schema") << 9000 << true << QString("x");
      QTest::newRow("oversized-definition") << 34000 << false << QString("x");
      QTest::newRow("utf8-budget") << 12000 << false << QStringLiteral("读");
  }
  void metadataGatewayPreservesCompleteDefinitions() {
      QFETCH(int, bodyLength);
      QFETCH(bool, expectedSuccess);
      QFETCH(QString, bodyCharacter);
      QTemporaryDir dir;
      const auto root = dir.filePath("package");
      makeTool(root);
      const QString body = QString(bodyLength, bodyCharacter[0]) + "CONSTRAINT_END";
      QFile usage(root + "/USAGE.md");
      QVERIFY(usage.open(QIODevice::WriteOnly));
      usage.write(body.toUtf8());
      usage.close();
      editTool(root, [](QJsonObject& object) {
          auto definition = object["definition"].toObject();
          auto operations = definition["operations"].toArray();
          auto operation = operations[0].toObject();
          operation["parameters"] = QJsonObject{{"source", "inline"}, {"schema", QJsonObject{
              {"type", "object"}, {"properties", QJsonObject{
                  {"token", QJsonObject{{"type", "string"}, {"description", "Public schema field"}}},
                  {"password", QJsonObject{{"type", "string"}}}}},
              {"required", QJsonArray{"token"}}, {"additionalProperties", false}}}};
          operations[0] = operation;
          definition["operations"] = operations;
          object["definition"] = definition;
      });
      PluginRuntime plugin("pet", dir.filePath("plugins.json"));
      QVERIFY(plugin.setHostConfiguration(hostConfig(root)));
      ToolRegistry registry;
      plugin.attach(&registry);
      ToolRuntime runtime;
      runtime.setToolRegistry(&registry);
      ToolExecutionRequest request;
      request.requestId = "metadata-test";
      request.toolName = "load_capability";
      request.context = ContextSpec::localOwner("pet");
      request.arguments = {{"ref", CapabilityRef{"example.plugin", "example.tool", "1", "probe"}.toJson()}};
      std::optional<ToolExecutionOutcome> outcome;
      runtime.executeAsync(request, [&](auto value) { outcome = value; });
      QTRY_VERIFY(outcome.has_value());
      QCOMPARE(outcome->result.success, expectedSuccess);
      if (!expectedSuccess) {
          QVERIFY(outcome->result.errorMessage.contains("CAPABILITY_METADATA_TOO_LARGE"));
          QVERIFY(outcome->result.data.isEmpty());
          // Direct host gateway callers receive the same bounded result.
          outcome.reset();
          plugin.executeGateway(request, [&](auto value) { outcome = value; });
          QTRY_VERIFY(outcome.has_value());
          QVERIFY(!outcome->result.success);
          return;
      }
      QCOMPARE(outcome->result.data["instructions"].toString(), body);
      const auto payload = QJsonDocument::fromJson(runtime.sanitizer()->toPayload(outcome->result).toUtf8()).object();
      QCOMPARE(payload["data"].toObject()["instructions"].toString(), body);
      const auto properties = payload["data"].toObject()["parameters"].toObject()["properties"].toObject();
      QCOMPARE(properties["token"].toObject()["type"].toString(), "string");
      QCOMPARE(properties["password"].toObject()["type"].toString(), "string");
      QVERIFY(!payload.contains("capabilityMetadata"));
  }
  void
  internalActivity_explicitAutonomousGrant_propagatesThroughActualToolExecution() {
      QTemporaryDir dir;
      const auto root = dir.filePath("package");
      makeTool(root);
      PluginRuntime runtime("pet", dir.filePath("plugins.json"));
      auto config = hostConfig(root);
      auto permission = config["grants"].toArray()[0].toObject();
      permission.remove("conversationId");
      permission["scope"] = "internal_activity";
      permission["autonomous"] = true;
      config["grants"] = QJsonArray{permission};
      QVERIFY(runtime.setHostConfiguration(config));
      ContextSpec context;
      context.profileId = "pet";
      QVERIFY(!runtime.discoverCapabilities(context, true).isEmpty());
      const CapabilityRef ref{"example.plugin", "example.tool", "1", "probe"};
      QVERIFY(runtime.makeCapabilityValidator()({ref}, context, true,
                                                runtime.catalogRevision()));
      auto loaded = runtime.loadCapability(ref, context, true);
      QVERIFY(loaded);
      ToolExecutionRequest call;
      call.requestId = "internal-call";
      call.activityId = "internal-activity";
      call.context = context;
      call.autonomous = true;
      call.toolName = "invoke_capability";
      call.arguments = {{"ref", ref.toJson()},
                        {"arguments", QJsonObject{{"value", "hello"}}}};
      std::optional<ToolExecutionOutcome> outcome;
      runtime.executeGateway(call, [&](auto value) { outcome = value; });
      QTRY_VERIFY(outcome.has_value());
      QVERIFY2(outcome->result.success,
               qPrintable(outcome->result.errorMessage));
      QVERIFY(outcome->executed);
      permission["autonomous"] = false;
      config["grants"] = QJsonArray{permission};
      QVERIFY(runtime.setHostConfiguration(config));
      QVERIFY(runtime.discoverCapabilities(context, true).isEmpty());
      QVERIFY(!runtime.makeCapabilityValidator()({ref}, context, true,
                                                 runtime.catalogRevision()));
      QVERIFY(!runtime.loadCapability(ref, context, true));
      permission["conversationId"] = "local_owner";
      config["grants"] = QJsonArray{permission};
      QVERIFY(!runtime.setHostConfiguration(config));
      QVERIFY(runtime.setHostConfiguration(hostConfig(root)));
      QVERIFY(runtime.discoverCapabilities(context, true).isEmpty());
  }

  void
  activityValidatorUsesCurrentGrantsVersionAndRetirementWithoutQObjectAccess() {
      QTemporaryDir dir;
      const auto root = dir.filePath("package");
      makeTool(root);
      auto runtime =
          std::make_unique<PluginRuntime>("pet", dir.filePath("plugins.json"));
      auto config = hostConfig(root);
      QVERIFY(runtime->setHostConfiguration(config));
      const auto validator = runtime->makeCapabilityValidator();
      const CapabilityRef ref{"example.plugin", "example.tool", "1", "probe"};
      const auto local = ContextSpec::localOwner("pet");
      QVERIFY(validator({ref}, local, false, 0));
      auto stale = ref;
      stale.entryVersion = "old";
      auto result = validator({stale}, local, false, 0);
      QVERIFY(!result);
      QCOMPARE(result.error().code, "STALE_REVISION");
      QVERIFY(runtime->setPluginEnabled(
          {ref.pluginId, false, runtime->catalogRevision()}));
      result = validator({ref}, local, false, 0);
      QVERIFY(!result);
      QCOMPARE(result.error().code, "PLUGIN_DISABLED");
      QVERIFY(runtime->setPluginEnabled(
          {ref.pluginId, true, runtime->catalogRevision()}));
      auto restricted = local;
      restricted.originKind = OriginKind::Internal;
      result = validator({ref}, restricted, false, 0);
      QVERIFY(!result);
      QCOMPARE(result.error().code, "PERMISSION_DENIED");
      restricted.originKind = OriginKind::External;
      restricted.conversationId = "room";
      auto permission = config["grants"].toArray()[0].toObject();
      permission["conversationId"] = restricted.conversationId;
      config["grants"] = QJsonArray{permission};
      QVERIFY(runtime->setHostConfiguration(config));
      QVERIFY(validator({ref}, restricted, false, 0));
      permission["executor"] = "different-executable";
      config["grants"] = QJsonArray{permission};
      QVERIFY(runtime->setHostConfiguration(config));
      result = validator({ref}, restricted, false, 0);
      QVERIFY(!result);
      QCOMPARE(result.error().code, "PERMISSION_DENIED");
      runtime.reset();
      result = validator({ref}, local, false, 0);
      QVERIFY(!result);
      QCOMPARE(result.error().code, "CANCELLED");
  }
    void hostConfiguration_duplicateIdentitiesAndDisabledUpgrade_areAtomic() {
        QTemporaryDir dir; const auto root=dir.filePath("package"); makeTool(root);
        PluginRuntime runtime("pet",dir.filePath("plugins.json")); const auto config=hostConfig(root);
        QVERIFY(runtime.setHostConfiguration(config)); const auto revision=runtime.catalogRevision();
        auto duplicate=config; duplicate["plugins"]=QJsonArray{config["plugins"].toArray()[0],config["plugins"].toArray()[0]};
        QVERIFY(!runtime.setHostConfiguration(duplicate)); QCOMPARE(runtime.catalogRevision(),revision);
        duplicate=config; duplicate["grants"]=QJsonArray{config["grants"].toArray()[0],config["grants"].toArray()[0]};
        QVERIFY(!runtime.setHostConfiguration(duplicate)); QCOMPARE(runtime.catalogRevision(),revision);
        auto missing=config; auto grant=missing["grants"].toArray()[0].toObject(); grant.remove("pluginId"); missing["grants"]=QJsonArray{grant};
        QVERIFY(!runtime.setHostConfiguration(missing)); QCOMPARE(runtime.catalogRevision(),revision);
        QVERIFY(runtime.setPluginEnabled({"example.plugin",false,revision}));
        QVERIFY(runtime.registerPlugin({root}));
        QCOMPARE(runtime.discoverCapabilities(ContextSpec::localOwner("pet"))[0].toObject()["readiness"].toString(),"disabled");
        QVERIFY(!runtime.loadCapability({"example.plugin","example.tool","1","probe"},ContextSpec::localOwner("pet")));
        PluginRuntime restored("pet",dir.filePath("plugins.json")); QVERIFY(restored.initialize());
        QCOMPARE(restored.discoverCapabilities(ContextSpec::localOwner("pet"))[0].toObject()["readiness"].toString(),"disabled");
        QVERIFY(!restored.loadCapability({"example.plugin","example.tool","1","probe"},ContextSpec::localOwner("pet")));
    }
    void learnedSkills_localOwnerCanLoad_externalAndInternalCannot() {
        QTemporaryDir dir; PluginRuntime runtime("pet",dir.filePath("plugins.json"));
        SkillEntry skill; skill.id="learned-workflow"; skill.name="Learned method"; skill.description="A reusable workflow";
        skill.abstractGoal="PRIVATE_LEARNED_BODY"; skill.steps.append({"PRIVATE_STEP","query","when needed"});
        runtime.setLearnedSkills({skill});
        const auto local=ContextSpec::localOwner("pet"); const CapabilityRef ref{"builtin.learned",skill.id,"1",{}};
        const auto summary=runtime.discoverCapabilities(local); QCOMPARE(summary.size(),1);
        QVERIFY(!QJsonDocument(summary).toJson().contains("PRIVATE_LEARNED_BODY"));
        const auto loaded=runtime.loadCapability(ref,local); QVERIFY(loaded);
        QVERIFY(loaded.value().instructions.contains("PRIVATE_LEARNED_BODY")); QVERIFY(loaded.value().instructions.contains("PRIVATE_STEP"));
        for (const auto origin: {OriginKind::External,OriginKind::Internal}) {
            auto restricted=local; restricted.originKind=origin; restricted.conversationId="other";
            QVERIFY(runtime.discoverCapabilities(restricted).isEmpty()); QVERIFY(!runtime.loadCapability(ref,restricted));
        }
        auto otherProfile=local; otherProfile.profileId="other-pet";
        QVERIFY(runtime.discoverCapabilities(otherProfile).isEmpty()); QVERIFY(!runtime.loadCapability(ref,otherProfile));
    }
    void registerPlugin_mixedDependenciesAndConflict_loadsOnlySelectedDetails() {
        QTemporaryDir dir; const auto root=dir.filePath("package"); QDir().mkpath(root); makeSkill(root); makeTool(root);
        writeJson(root+"/plugin.json",{{"formatVersion","1"},{"id","example.plugin"},{"name","Mixed"},{"version","1"},{"summary","Tools and methods"},
            {"entries",QJsonArray{QJsonObject{{"kind","skill"},{"id","example.method"},{"definition","skill.json"}},QJsonObject{{"kind","tool"},{"id","example.tool"},{"definition","tool.json"}}}}});
        QFile f(root+"/skill.json"); QVERIFY(f.open(QIODevice::ReadOnly)); auto skill=QJsonDocument::fromJson(f.readAll()).object(); f.close();
        skill["dependencies"]=QJsonArray{QJsonObject{{"kind","tool"},{"id","example.tool"},{"version","1"},{"operations",QJsonArray{"probe"}},{"required",true}}}; writeJson(f.fileName(),skill);
        PluginCatalog catalog; auto registered=catalog.registerPlugin({root}); QVERIFY(registered); QCOMPARE(registered.value().entries.size(),2);
        auto loaded=catalog.loadCapability({"example.plugin","example.method","1",{}}); QVERIFY(loaded); QVERIFY(loaded.value().operation.isEmpty()); QVERIFY(!loaded.value().toPublicJson().contains("parameters"));
        QTemporaryDir another; makeTool(another.path()); QFile plugin(another.filePath("plugin.json")); QVERIFY(plugin.open(QIODevice::ReadOnly)); auto manifest=QJsonDocument::fromJson(plugin.readAll()).object(); plugin.close(); manifest["id"]="different.plugin"; writeJson(plugin.fileName(),manifest);
        QVERIFY(!catalog.registerPlugin({another.path()})); QCOMPARE(catalog.discover().size(),2);
        auto dependency=skill["dependencies"].toArray()[0].toObject(); dependency["version"]="2"; skill["dependencies"]=QJsonArray{dependency}; writeJson(f.fileName(),skill);
        QVERIFY(catalog.registerPlugin({root})); QVERIFY(!catalog.loadCapability({"example.plugin","example.method","1",{}}));
    }
    void invokeCapability_mcpErrorsAndActualSchema_areNotEmptySuccess() {
        QTemporaryDir dir; const auto root=dir.filePath("package"); makeTool(root,"mcp"); PluginRuntime runtime("pet",dir.filePath("plugins.json")); QVERIFY(runtime.setHostConfiguration(hostConfig(root,"mcp")));
        auto loaded=runtime.loadCapability({"example.plugin","example.tool","1","probe"},ContextSpec::localOwner("pet")); QVERIFY(loaded);
        for (const auto& mode:{QString("tool_error"),QString("protocol_error"),QString("disconnect"),QString("timeout"),QString("invalid")}) {
            auto call=makeCall(loaded.value()); call.arguments=mode=="invalid"?QJsonObject{{"extra",true}}:QJsonObject{{"mode",mode}}; if (mode=="timeout") call.timeoutMs=200;
            std::optional<CapabilityOutcome> outcome; runtime.invokeCapability(call,[&](CapabilityOutcome v){outcome=v;}); QTRY_VERIFY(outcome);
            QCOMPARE(outcome->stage,"failed"); QVERIFY(!outcome->error.code.isEmpty()); if (mode=="invalid") QVERIFY(!outcome->executed);
        }
    }
    void invokeCapability_objectAndPathGrants_areCheckedBeforeSpawn() {
        QTemporaryDir dir; const auto root=dir.filePath("package"); makeTool(root); auto config=hostConfig(root);
        auto permission=config["grants"].toArray()[0].toObject(); permission["objectArguments"]=QJsonObject{{"/target","room-A"}}; permission["pathArguments"]=QJsonArray{"/path"}; permission["allowedRootPaths"]=QJsonArray{root}; config["grants"]=QJsonArray{permission};
        PluginRuntime runtime("pet",dir.filePath("plugins.json")); QVERIFY(runtime.setHostConfiguration(config)); auto loaded=runtime.loadCapability({"example.plugin","example.tool","1","probe"},ContextSpec::localOwner("pet")); QVERIFY(loaded);
        for (const auto& args:QList<QJsonObject>{{{"target","room-B"},{"path",root+"/USAGE.md"}},{{"target","room-A"},{"path",dir.filePath("outside")}}}) {
            auto call=makeCall(loaded.value()); call.arguments=args; std::optional<CapabilityOutcome> outcome; runtime.invokeCapability(call,[&](CapabilityOutcome v){outcome=v;}); QTRY_VERIFY(outcome); QVERIFY(!outcome->executed); QCOMPARE(outcome->error.code,"PERMISSION_DENIED");
        }
    }
    void gateway_disableQueuedInvocation_preservesInFlightCall() {
        QTemporaryDir dir; const auto root=dir.filePath("package"); makeTool(root); PluginRuntime plugin("pet",dir.filePath("plugins.json")); QVERIFY(plugin.setHostConfiguration(hostConfig(root)));
        ToolRegistry registry; plugin.attach(&registry); ToolRuntime runtime; runtime.setToolRegistry(&registry);
        auto loaded=plugin.loadCapability({"example.plugin","example.tool","1","probe"},ContextSpec::localOwner("pet")); QVERIFY(loaded);
        ToolExecutionRequest call; call.requestId="first"; call.toolName="invoke_capability"; call.context=ContextSpec::localOwner("pet"); call.activityId="active"; call.arguments={{"ref",loaded.value().ref.toJson()},{"arguments",QJsonObject{{"mode","slow"}}}};
        QSignalSpy started(&plugin,&PluginRuntime::capabilityDispatching); QList<ToolExecutionOutcome> outcomes;
        runtime.executeAsync(call,[&](ToolExecutionOutcome v){outcomes.append(v);}); QTRY_VERIFY(!started.isEmpty());
        call.requestId="second"; runtime.executeAsync(call,[&](ToolExecutionOutcome v){outcomes.append(v);}); registry.setToolEnabled("invoke_capability",false);
        QTRY_COMPARE(outcomes.size(),2); QCOMPARE(outcomes[0].requestId,"first"); QVERIFY(outcomes[0].executed); QCOMPARE(outcomes[1].requestId,"second"); QVERIFY(!outcomes[1].executed);
    }
    void makeReplyDispatcher_usesRemoteReferenceAndActualReceipt() {
        QTemporaryDir dir; const auto root=dir.filePath("package"); makeTool(root); PluginRuntime runtime("pet",dir.filePath("plugins.json")); QVERIFY(runtime.setHostConfiguration(hostConfig(root)));
        CapabilityReplyBinding binding; binding.ref={"example.plugin","example.tool","1","probe"}; binding.grantRef="test-grant";
        binding.argumentBindings={{"/message/text","content"},{"/target","remoteConversationId"},{"/reply","replyToRemoteId"}};
        ReplyDispatch dispatch; dispatch.dispatchToken=std::make_shared<ToolDispatchToken>(); dispatch.response.requestId="reply-request"; dispatch.response.generation=12;
        dispatch.response.context=ContextSpec::localOwner("pet"); dispatch.response.replyTarget.remoteConversationId="room-A"; dispatch.response.replyTarget.replyToMessageId="local-id"; dispatch.response.content="proposed"; dispatch.arguments={{"reply_to_message_id","remote-original"}};
        std::optional<DeliveryReceipt> receipt; runtime.makeReplyDispatcher(binding)(dispatch,[&](DeliveryReceipt v){receipt=v;}); QTRY_VERIFY(receipt);
        QCOMPARE(receipt->outcome,DeliveryOutcome::Completed); QCOMPARE(receipt->content,"backend-confirmed"); QCOMPARE(receipt->remoteMessageId,"remote-123"); QCOMPARE(receipt->generation,12);
        const auto echo=receipt->evidence["data"].toObject()["echo"].toObject(); QCOMPARE(echo["reply"].toString(),"remote-original"); QCOMPARE(echo["message"].toObject()["text"].toString(),"proposed");
        QCOMPARE(receipt->occurredAt.toUTC().toString(Qt::ISODateWithMs),"2026-10-16T08:00:00.000Z");
    }
    void schema_rejectsInvalidMetaschemaVocabularyAndTrailingIdentifier() {
        for (const auto& bad:QList<QJsonObject>{{{"multipleOf",0}},{{"minItems",-1}},{{"type","not-a-type"}},{{"required",true}},
                {{"$vocabulary",QJsonObject{{"https://json-schema.org/draft/2020-12/vocab/nonexistent",true}}}}})
            QVERIFY2(!SchemaValidator::check(bad),qPrintable(QString::fromUtf8(QJsonDocument(bad).toJson())));
        QTemporaryDir dir; makeSkill(dir.path()); QFile f(dir.filePath("plugin.json")); QVERIFY(f.open(QIODevice::ReadOnly));
        auto manifest=QJsonDocument::fromJson(f.readAll()).object(); manifest["id"]="example.plugin\n";
        QVERIFY(!SchemaValidator::validateManifest("plugin",manifest));
        const QJsonObject valid{{"type","object"},{"properties",QJsonObject{{"unknownConstraint",QJsonObject{{"type","string"}}}}}};
        QVERIFY(SchemaValidator::validate(valid,QJsonObject{{"unknownConstraint","business property"}}));
    }
    void schema_localReferencesAndConditionalConstraints() {
        QTemporaryDir dir; writeJson(dir.filePath("number.json"),{{"type","integer"},{"minimum",4}});
        const QJsonObject schema{{"type","object"},{"properties",QJsonObject{{"n",QJsonObject{{"$ref","number.json"}}},{"kind",QJsonObject{{"enum",QJsonArray{"a","b"}}}}}},
            {"if",QJsonObject{{"properties",QJsonObject{{"kind",QJsonObject{{"const","a"}}}}}}},
            {"then",QJsonObject{{"required",QJsonArray{"n"}}}},{"unevaluatedProperties",false}};
        QVERIFY(SchemaValidator::validate(schema,QJsonObject{{"kind","a"},{"n",4}},dir.path(),dir.filePath("schema.json")));
        QVERIFY(!SchemaValidator::validate(schema,QJsonObject{{"kind","a"},{"n",3}},dir.path(),dir.filePath("schema.json")));
        QVERIFY(!SchemaValidator::validate(schema,QJsonObject{{"kind","a"}},dir.path(),dir.filePath("schema.json")));
        QTemporaryDir outside; writeJson(outside.filePath("outside.json"),{{"type","integer"}});
        QVERIFY(QFile::link(outside.filePath("outside.json"),dir.filePath("escape.json")));
        QVERIFY(!SchemaValidator::check(QJsonObject{{"$ref","escape.json"}},dir.path(),dir.filePath("schema.json")));
        QVERIFY(!SchemaValidator::check(QJsonObject{{"$ref","https://example.com/schema"}}));
    }
    void invokeCapability_outcomesAndPreflightAdapters() {
        QTemporaryDir dir; const auto root=dir.filePath("package"); makeTool(root);
        PluginRuntime runtime("pet",dir.filePath("plugins.json")); auto config=hostConfig(root);
        for (const auto& mode: {QString("empty"),QString("scalar"),QString("fail"),QString("accepted"),QString("reported_failure")}) {
            resultCompletion(root,mode=="accepted"?"accepted":mode=="reported_failure"?"reported":"completed");
            config["resultAdapters"]=QJsonObject{{"example.tool/probe",QJsonObject{{"statePointer","/status"},{"stateMap",QJsonObject{{"done","completed"},{"queued","accepted"},{"bad","failed"}}},{"receiptPointer","/receipt"},{"trackingPointer","/tracking"}}}};
            QVERIFY(runtime.setHostConfiguration(config)); auto loaded=runtime.loadCapability({"example.plugin","example.tool","1","probe"},ContextSpec::localOwner("pet")); QVERIFY(loaded);
            auto call=makeCall(loaded.value()); call.arguments={{"mode",mode}}; std::optional<CapabilityOutcome> result;
            runtime.invokeCapability(call,[&](CapabilityOutcome v){result=v;}); QTRY_VERIFY(result);
            QCOMPARE(result->stage,mode=="accepted"?QString("accepted"):mode=="fail" || mode=="reported_failure"?QString("failed"):QString("completed"));
            if (mode=="scalar") QCOMPARE(result->data.toInt(),42);
        }
        config.remove("resultAdapters"); QVERIFY(runtime.setHostConfiguration(config));
        auto loaded=runtime.loadCapability({"example.plugin","example.tool","1","probe"},ContextSpec::localOwner("pet")); QVERIFY(loaded);
        std::optional<CapabilityOutcome> missing; runtime.invokeCapability(makeCall(loaded.value()),[&](CapabilityOutcome v){missing=v;});
        QTRY_VERIFY(missing); QVERIFY(!missing->executed); QCOMPARE(missing->error.code,"DEPENDENCY_UNAVAILABLE");
    }
    void invokeCapability_grantsScopeAndSecretEcho() {
        QTemporaryDir dir; const auto root=dir.filePath("package"); makeTool(root); auto config=hostConfig(root);
        PluginRuntime runtime("pet",dir.filePath("plugins.json")); QVERIFY(runtime.setHostConfiguration(config));
        ContextSpec external=ContextSpec::localOwner("pet"); external.originKind=OriginKind::External;
        QVERIFY(runtime.discoverCapabilities(external).isEmpty());
        QVERIFY(!runtime.loadCapability({"example.plugin","example.tool","1","probe"},external));
        editTool(root,[](QJsonObject& object) {auto definition=object["definition"].toObject(); auto settings=definition["configuration"].toArray();
            settings.append(QJsonObject{{"key","token"},{"kind","secret"},{"required",true},{"description","Private fixture token"}}); definition["configuration"]=settings;
            auto operations=definition["operations"].toArray(); auto op=operations[0].toObject(); auto invocation=op["invocation"].toObject();
            invocation["arguments"]=QJsonArray{QJsonObject{{"setting","script"}},"--slow-args",QJsonObject{{"setting","token"}}}; op["invocation"]=invocation; operations[0]=op; definition["operations"]=operations; object["definition"]=definition; });
        auto configurations=config["configuration"].toObject(); auto settings=configurations["example.tool"].toObject(); settings["token"]="host-secret-123"; configurations["example.tool"]=settings; config["configuration"]=configurations;
        QVERIFY(runtime.setHostConfiguration(config)); auto loaded=runtime.loadCapability({"example.plugin","example.tool","1","probe"},ContextSpec::localOwner("pet")); QVERIFY(loaded);
        QSignalSpy started(&runtime,&PluginRuntime::capabilityDispatching);
        std::optional<CapabilityOutcome> result; runtime.invokeCapability(makeCall(loaded.value()),[&](CapabilityOutcome v){result=v;}); QTRY_VERIFY(!started.isEmpty());
        settings["token"]="rotated-secret-456"; configurations["example.tool"]=settings; config["configuration"]=configurations; QVERIFY(runtime.setHostConfiguration(config));
        QTRY_VERIFY(result); QVERIFY(!QJsonDocument(result->toJson()).toJson().contains("host-secret-123"));
    }
    void setPluginEnabled_inFlightReceiptAndQueuedCalls_keepIdentity() {
        QTemporaryDir dir; const auto root=dir.filePath("package"); makeTool(root);
        auto runtime=std::make_unique<PluginRuntime>("pet",dir.filePath("plugins.json")); QVERIFY(runtime->setHostConfiguration(hostConfig(root)));
        auto loaded=runtime->loadCapability({"example.plugin","example.tool","1","probe"},ContextSpec::localOwner("pet")); QVERIFY(loaded);
        auto call=makeCall(loaded.value()); call.arguments={{"mode","slow"}};
        QSignalSpy started(runtime.get(),&PluginRuntime::capabilityDispatching);
        QList<CapabilityOutcome> results; runtime->invokeCapability(call,[&](CapabilityOutcome v){results.append(v);});
        QTRY_VERIFY(!started.isEmpty()); call.callId="queued-call"; runtime->invokeCapability(call,[&](CapabilityOutcome v){results.append(v);});
        QVERIFY(runtime->setPluginEnabled({"example.plugin",false,runtime->catalogRevision()}));
        QTRY_COMPARE(results.size(),2); QCOMPARE(results[0].callId,"call-1"); QCOMPARE(results[0].activityId,"activity-1"); QCOMPARE(results[0].stage,"completed");
        QCOMPARE(results[1].callId,"queued-call"); QVERIFY(!results[1].executed);
    }
    void invokeCapability_scriptCliAndControlledMcp_returnsActualOutcome() {
        for (const auto& kind: {QString("command"),QString("mcp")}) {
            QTemporaryDir dir; const auto root=dir.filePath("package"); makeTool(root,kind);
            PluginRuntime runtime("pet",dir.filePath("plugins.json")); QVERIFY(runtime.setHostConfiguration(hostConfig(root,kind)));
            auto loaded=runtime.loadCapability({"example.plugin","example.tool","1","probe"},ContextSpec::localOwner("pet"));
            QVERIFY2(loaded,qPrintable(loaded?QString():loaded.error().message));
            auto call=makeCall(loaded.value()); call.arguments={{"mode","empty"}};
            std::optional<CapabilityOutcome> outcome; int callbacks=0;
            runtime.invokeCapability(call,[&](CapabilityOutcome value){ outcome=value; ++callbacks; });
            QTRY_VERIFY(outcome.has_value()); QCOMPARE(callbacks,1); QCOMPARE(outcome->stage,"completed"); QVERIFY(outcome->executed);
            if (kind=="command") QVERIFY(outcome->data.isArray());
            else QCOMPARE(outcome->data.toObject()["content"].toArray().size(),2);
        }
    }
    void setPluginEnabled_disableThenEnable_blocksNewCallsAndInvalidatesOldCache() {
        QTemporaryDir dir; const auto root=dir.filePath("package"); makeTool(root);
        PluginRuntime runtime("pet",dir.filePath("plugins.json")); QVERIFY(runtime.setHostConfiguration(hostConfig(root)));
        auto loaded=runtime.loadCapability({"example.plugin","example.tool","1","probe"},ContextSpec::localOwner("pet")); QVERIFY(loaded);
        auto call=makeCall(loaded.value());
        QVERIFY(runtime.setPluginEnabled({"example.plugin",false,runtime.catalogRevision()}));
        std::optional<CapabilityOutcome> outcome;
        runtime.invokeCapability(call,[&](CapabilityOutcome v){outcome=v;}); QTRY_VERIFY(outcome); QVERIFY(!outcome->executed); QCOMPARE(outcome->error.code,"PLUGIN_DISABLED");
        QVERIFY(runtime.setPluginEnabled({"example.plugin",true,runtime.catalogRevision()})); outcome.reset();
        runtime.invokeCapability(call,[&](CapabilityOutcome v){outcome=v;}); QTRY_VERIFY(outcome); QVERIFY(!outcome->executed); QCOMPARE(outcome->error.code,"STALE_REVISION");
    }
    void registerPlugin_skillOnly_indexesSummaryAndLoadsOnDemand() {
        QTemporaryDir dir; makeSkill(dir.path()); PluginCatalog catalog;
        auto result = catalog.registerPlugin({dir.path()});
        QVERIFY2(result, qPrintable(result ? QString() : result.error().message));
        QCOMPARE(result.value().entries.size(), 1);
        QVERIFY(!QJsonDocument(catalog.discover()).toJson().contains("PRIVATE_METHOD_BODY"));
        auto loaded = catalog.loadCapability({"example.plugin", "example.method", "1", {}});
        QVERIFY(loaded); QVERIFY(loaded.value().instructions.contains("PRIVATE_METHOD_BODY"));
    }
    void schema_standard202012AndUnknownKeyword() {
        const QJsonObject schema{{"type", "object"}, {"properties", QJsonObject{{"n", QJsonObject{{"type", "integer"}, {"minimum", 2}}}}},
            {"required", QJsonArray{"n"}}, {"unevaluatedProperties", false}};
        QVERIFY(SchemaValidator::validate(schema, QJsonObject{{"n", 2}}));
        QVERIFY(!SchemaValidator::validate(schema, QJsonObject{{"n", 1}}));
        QVERIFY(!SchemaValidator::validate(schema, QJsonObject{{"n", 2}, {"other", true}}));
        auto unsupported = schema; unsupported["unknownConstraint"] = true;
        auto result = SchemaValidator::check(unsupported);
        QVERIFY(!result); QCOMPARE(result.error().code, "SCHEMA_UNSUPPORTED");
    }
    void registerPlugin_rejectsEscapingDefinition() {
        QTemporaryDir dir; makeSkill(dir.path());
        QFile file(dir.filePath("plugin.json")); QVERIFY(file.open(QIODevice::ReadOnly));
        auto object = QJsonDocument::fromJson(file.readAll()).object(); file.close();
        object["entries"] = QJsonArray{QJsonObject{{"kind", "skill"}, {"id", "example.method"}, {"definition", "../skill.json"}}};
        writeJson(file.fileName(), object); PluginCatalog catalog;
        QVERIFY(!catalog.registerPlugin({dir.path()})); QVERIFY(catalog.discover().isEmpty());
    }
};
QTEST_GUILESS_MAIN(PluginRuntimeTests)
#include "test_plugin_runtime.moc"
