#include "ai/plugin/capability_context.h"
#include <QJsonDocument>
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
}

class CapabilityContextTests : public QObject {
    Q_OBJECT
private slots:
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
