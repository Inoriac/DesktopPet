#include "ai/plugin/capability_context.h"
#include <QJsonDocument>
#include <QRegularExpression>
#include <QtTest>

namespace {
QJsonObject ref(const QString& entry, const QString& operation = {}) {
    QJsonObject value{{"pluginId", "test.plugin"}, {"entryId", entry}, {"entryVersion", "1"}};
    if (!operation.isEmpty()) value["operationId"] = operation;
    return value;
}
QJsonObject loaded(const QString& entry, const QString& operation = {}, const QString& kind = "tool") {
    QJsonObject value{{"ref", ref(entry, operation)}, {"kind", kind},
        {"catalogRevision", 1}, {"definitionDigest", "digest-1"},
        {"instructions", "COMMON_BODY_SENTINEL\n" + QString(2500, QLatin1Char('x'))}};
    if (kind == "skill") value["dependencies"] = QJsonArray{ref("browser", "read")};
    else if (operation.isEmpty()) value["operations"] = QJsonArray{QJsonObject{{"id", "read"}}};
    else {
        value["parameters"] = QJsonObject{{"type", "object"}, {"properties", QJsonObject{
            {"token", QJsonObject{{"type", "string"}}}}}};
        value["effects"] = QJsonObject{{"kind", "read"}, {"description", "Read only"}};
        value["result"] = QJsonObject{{"successMeaning", "Actual completion"}};
    }
    return value;
}
ChatMessage message(const QString& name, const QJsonObject& data, const QString& id) {
    ChatMessage value;
    value.role = "tool"; value.name = name; value.toolCallId = id;
    value.content = QString::fromUtf8(QJsonDocument(QJsonObject{{"success", true}, {"data", data}}).toJson(QJsonDocument::Compact));
    return value;
}
QJsonObject data(const ChatMessage& value) {
    return QJsonDocument::fromJson(value.content.toUtf8()).object()["data"].toObject();
}
QJsonObject step(const QString& name, const QJsonObject& output, const QString& id) {
    return {{"id", id}, {"name", name}, {"status", "succeeded"}, {"output", output}};
}
QByteArray serialized(const QList<ChatMessage>& messages) {
    QJsonArray values;
    for (const auto& value : messages)
        values.append(QJsonObject{{"role", value.role}, {"name", value.name}, {"tool_call_id", value.toolCallId},
            {"content", value.content}, {"tool_calls", value.toolCalls}, {"transportBlocks", value.transportBlocks}});
    return QJsonDocument(values).toJson(QJsonDocument::Compact);
}
QJsonArray gatewayTools() {
    return {QJsonObject{{"type", "function"}, {"function", QJsonObject{{"name", "discover_capabilities"}}}},
            QJsonObject{{"type", "function"}, {"function", QJsonObject{{"name", "load_capability"}}}},
            QJsonObject{{"type", "function"}, {"function", QJsonObject{{"name", "invoke_capability"}}}}};
}
}

class CapabilityContextTests : public QObject {
    Q_OBJECT
private slots:
    void concreteToolsAreRequestScopedAndPreserveBusinessParameters() {
        const QJsonObject native{{"type", "function"}, {"function", QJsonObject{
            {"name", "web_search"}, {"parameters", QJsonObject{{"type", "object"}}}}}};
        auto base = gatewayTools();
        base.append(native);
        const auto empty = CapabilityContext::projectTools(base, QList<ChatMessage>{});
        QCOMPARE(empty.tools.size(), 3);
        QVERIFY(empty.operationRefs.isEmpty());
        auto read = loaded("browser", "read_page");
        read["parameters"] = QJsonObject{{"type", "object"}, {"properties", QJsonObject{
            {"url", QJsonObject{{"type", "string"}}}, {"cursor", QJsonObject{{"type", "string"}}}}},
            {"required", QJsonArray{"url"}}, {"additionalProperties", false}};
        auto search = loaded("browser", "search");
        search["parameters"] = QJsonObject{{"type", "object"}, {"properties", QJsonObject{
            {"query", QJsonObject{{"type", "string"}}}}}, {"required", QJsonArray{"query"}}};
        const auto messages = CapabilityContext::projectMessages({message("load_capability", search, "search"),
                                                                  message("load_capability", read, "read")});
        const auto projected = CapabilityContext::projectTools(base, messages);
        QCOMPARE(projected.operationRefs.size(), 2);
        QCOMPARE(projected.tools.size(), 5);
        QVERIFY(projected.tools.contains(native));
        const QString name = projected.operationRefs.key(ref("browser", "read_page"));
        QVERIFY(!name.isEmpty());
        for (const auto& item : projected.tools) {
            const auto function = item.toObject()["function"].toObject();
            QVERIFY(function["name"] != "invoke_capability");
            if (!projected.operationRefs.contains(function["name"].toString())) continue;
            QVERIFY(function["name"].toString().size() <= 64);
            QVERIFY(QRegularExpression("^[A-Za-z0-9_-]+$").match(function["name"].toString()).hasMatch());
            QVERIFY(!function["description"].toString().contains("COMMON_BODY_SENTINEL"));
            QCOMPARE(function["parameters"], function["name"] == name ? read["parameters"] : search["parameters"]);
        }
        LlmToolCall call{"call-id", "function", name, {{"url", "https://example.org"}, {"cursor", "opaque"}}};
        const auto business = call.arguments;
        QJsonObject binding;
        QVERIFY(projected.normalizeCall(call, &binding));
        QCOMPARE(binding["catalogRevision"], read["catalogRevision"]);
        QCOMPARE(binding["definitionDigest"], read["definitionDigest"]);
        QCOMPARE(call.id, "call-id");
        QCOMPARE(call.name, "invoke_capability");
        QCOMPARE(call.arguments["ref"].toObject(), ref("browser", "read_page"));
        QCOMPARE(call.arguments["arguments"].toObject(), business);
        LlmToolCall nativeCall{"native-id", "function", "web_search", {{"query", "same input"}}};
        QVERIFY(projected.normalizeCall(nativeCall, &binding));
        QVERIFY(binding.isEmpty());
        QCOMPARE(nativeCall.name, "web_search");
        QCOMPARE(nativeCall.arguments["query"].toString(), "same input");
        const auto task = CapabilityContext::projectTools(base, CapabilityContext::projectSteps({
            step("load_capability", search, "search"), step("load_capability", read, "read")}));
        QCOMPARE(task.tools, projected.tools);
        QCOMPARE(task.operationRefs, projected.operationRefs);
    }
    void releasedStaleAndForgedAliasesCannotNormalize() {
        const auto base = gatewayTools();
        QList<ChatMessage> records{message("load_capability", loaded("video", "read"), "one")};
        const auto before = CapabilityContext::projectTools(base, CapabilityContext::projectMessages(records));
        const auto oldName = before.operationRefs.key(ref("video", "read"));
        QVERIFY(!oldName.isEmpty());
        LlmToolCall call{"id", "function", oldName, {}};
        QVERIFY(!CapabilityContext::projectTools(base, QList<ChatMessage>{}).normalizeCall(call));
        records.append(message("load_capability", loaded("browser", "search"), "two"));
        QVERIFY(!CapabilityContext::projectTools(base, CapabilityContext::projectMessages(records)).normalizeCall(call));
        auto revision = loaded("video", "read");
        revision["catalogRevision"] = 2;
        revision["definitionDigest"] = "revised";
        records.append(message("load_capability", revision, "three"));
        const auto current = CapabilityContext::projectTools(base, CapabilityContext::projectMessages(records));
        QVERIFY(!current.normalizeCall(call));
        QVERIFY(current.operationRefs.key(ref("video", "read")) != oldName);
        call.name = "cap_forged";
        QVERIFY(!current.normalizeCall(call));
        call.name = "invoke_capability";
        QVERIFY(!current.normalizeCall(call));
        const auto noGateway = CapabilityContext::projectTools({}, CapabilityContext::projectMessages(records));
        QVERIFY(noGateway.operationRefs.isEmpty());
        revision.remove("instructions");
        revision["instructionsRef"] = "missing-body";
        QVERIFY(CapabilityContext::projectTools(base, QList<ChatMessage>{message("load_capability", revision, "orphan")})
                    .operationRefs.isEmpty());
    }
    void sharedInstructionsAppearOnceAndProtocolIsUntouched() {
        ChatMessage assistant;
        assistant.role = "assistant";
        assistant.content = "working";
        assistant.toolCalls = QJsonArray{QJsonObject{{"id", "one"}, {"function", QJsonObject{{"name", "load_capability"}}}},
                                        QJsonObject{{"id", "two"}, {"function", QJsonObject{{"name", "load_capability"}}}}};
        assistant.transportBlocks = QJsonArray{QJsonObject{{"type", "thinking"}, {"signature", "signed-original"}, {"thinking", "provider thinking"}}};
        QList<ChatMessage> original{assistant, message("load_capability", loaded("video", "open"), "one"),
                                             message("load_capability", loaded("video", "read"), "two")};
        const auto raw = serialized(original);
        const auto projected = CapabilityContext::projectMessages(original);
        QCOMPARE(serialized(original), raw);
        QCOMPARE(projected.size(), original.size());
        QCOMPARE(projected[0].toolCalls, assistant.toolCalls);
        QCOMPARE(projected[0].transportBlocks, assistant.transportBlocks);
        QCOMPARE(projected[1].toolCallId, "one");
        QCOMPARE(projected[2].toolCallId, "two");
        QCOMPARE(serialized(projected).count("COMMON_BODY_SENTINEL"), 1);
        for (int i = 1; i < projected.size(); ++i) {
            QCOMPARE(data(projected[i])["parameters"], data(original[i])["parameters"]);
            QCOMPARE(data(projected[i])["effects"], data(original[i])["effects"]);
            QCOMPARE(data(projected[i])["result"], data(original[i])["result"]);
        }
        QVERIFY(CapabilityContext::hasOperation(projected, ref("video", "open")));
        QVERIFY(CapabilityContext::hasOperation(projected, ref("video", "read")));
        QVERIFY(!CapabilityContext::hasOperation(projected, ref("video", "delete")));
        QCOMPARE(serialized(CapabilityContext::projectMessages(projected)), serialized(projected));
    }
    void switchingReloadingAndDefinitionRevisions() {
        QList<ChatMessage> records{
            message("load_capability", loaded("video"), "overview"),
            message("load_capability", loaded("video", "open"), "one"),
            message("load_capability", loaded("video", "pause"), "two"),
            message("load_capability", loaded("video", "read"), "three"),
            message("load_capability", loaded("method", {}, "skill"), "skill")};
        auto projected = CapabilityContext::projectMessages(records);
        QVERIFY(!data(projected[0]).contains("operations"));
        QVERIFY(!CapabilityContext::hasOperation(projected, ref("video", "open")));
        QVERIFY(CapabilityContext::hasOperation(projected, ref("video", "pause")));
        QVERIFY(CapabilityContext::hasOperation(projected, ref("video", "read")));
        records.append(message("load_capability", loaded("browser", "fetch"), "switch"));
        projected = CapabilityContext::projectMessages(records);
        QVERIFY(!CapabilityContext::hasOperation(projected, ref("video", "read")));
        QVERIFY(CapabilityContext::hasOperation(projected, ref("browser", "fetch")));
        QVERIFY(data(projected[4]).contains("dependencies"));
        records.append(message("load_capability", loaded("video", "open"), "reload"));
        projected = CapabilityContext::projectMessages(records);
        QVERIFY(CapabilityContext::hasOperation(projected, ref("video", "open")));
        QVERIFY(!CapabilityContext::hasOperation(projected, ref("browser", "fetch")));
        auto updated = loaded("video", "read");
        updated["definitionDigest"] = "digest-2";
        updated["catalogRevision"] = 2;
        records.append(message("load_capability", updated, "new-revision"));
        projected = CapabilityContext::projectMessages(records);
        QVERIFY(!CapabilityContext::hasOperation(projected, ref("video", "open")));
        QVERIFY(CapabilityContext::hasOperation(projected, ref("video", "read")));
        QVERIFY(data(projected.last()).contains("instructions"));
    }
    void onlyMetadataIsCompactedAndBusinessReceiptsRemainIntact() {
        QJsonArray steps{
            step("discover_capabilities", {{"entries", QJsonArray{ref("video")}}}, "discovery-1"),
            step("load_capability", loaded("video", "open"), "one"),
            step("load_capability", loaded("video", "read"), "two"),
            step("discover_capabilities", {{"entries", QJsonArray{ref("browser")}}}, "discovery-2")};
        for (const auto& status : {"failed", "accepted", "unknown"}) {
            auto business = step("invoke_capability", {{"stage", status}, {"error", "business detail"},
                {"tracking", QJsonObject{{"remoteJobId", "42"}}}, {"receipt", QJsonObject{{"remoteMessageId", "99"}}}}, status);
            business["status"] = status;
            steps.append(business);
        }
        auto failedLoad = step("load_capability", loaded("other", "invoke"), "failed-load");
        failedLoad["status"] = "failed";
        steps.append(failedLoad);
        const auto raw = QJsonDocument(steps).toJson(QJsonDocument::Compact);
        const auto projected = CapabilityContext::projectSteps(steps);
        QCOMPARE(QJsonDocument(steps).toJson(QJsonDocument::Compact), raw);
        QCOMPARE(QJsonDocument(projected).toJson(QJsonDocument::Compact).count("COMMON_BODY_SENTINEL"), 2); // selected body plus untouched failure
        QVERIFY(!projected[0].toObject()["output"].toObject().contains("entries"));
        QCOMPARE(projected[3], steps[3]);
        for (int i = 4; i < steps.size(); ++i) QCOMPARE(projected[i], steps[i]);
        QVERIFY(CapabilityContext::hasOperation(projected, ref("video", "read")));
        QVERIFY(!CapabilityContext::hasOperation(projected, ref("other", "invoke")));
    }
    void syntheticRequestSizeEvidence() {
        QList<ChatMessage> messages;
        QJsonArray steps;
        for (int i = 0; i < 12; ++i) {
            const auto value = loaded(i < 6 ? "video" : "browser", i % 2 ? "read" : "open");
            messages.append(message("load_capability", value, QString::number(i)));
            steps.append(step("load_capability", value, QString::number(i)));
        }
        const auto originalBytes = serialized(messages);
        const auto projectedBytes = serialized(CapabilityContext::projectMessages(messages));
        const auto originalSteps = QJsonDocument(steps).toJson(QJsonDocument::Compact);
        const auto projectedSteps = QJsonDocument(CapabilityContext::projectSteps(steps)).toJson(QJsonDocument::Compact);
        qInfo() << "Dialogue UTF-8 bytes" << originalBytes.size() << "->" << projectedBytes.size()
                << "instruction bodies" << originalBytes.count("COMMON_BODY_SENTINEL") << "->" << projectedBytes.count("COMMON_BODY_SENTINEL");
        qInfo() << "Task steps UTF-8 bytes" << originalSteps.size() << "->" << projectedSteps.size();
        QVERIFY(projectedBytes.size() < originalBytes.size() / 3);
        QCOMPARE(projectedBytes.count("COMMON_BODY_SENTINEL"), 1);
    }
};
QTEST_GUILESS_MAIN(CapabilityContextTests)
#include "test_capability_context.moc"
