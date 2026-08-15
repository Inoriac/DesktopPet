//
// Created by Huang_cj on 2026/4/8.
// Tool 系统测试
// 测试 AITool 基类、ToolRegistry 注册/查找/执行、Schema 导出
// 不依赖 OpenGL 或模型文件，纯内存运行
//

#include <QTest>
#include <QJsonDocument>
#include <QFile>
#include <QDir>
#include <QTemporaryDir>
#include <memory>

#include "tool_registry.h"
#include "tools/environment_tools.h"
#include "tools/runtime/tool_runtime.h"
#include "scheduler/agent_scheduler.h"

// ================================================================
// 用于测试的 Mock Tool
// 不依赖任何外部系统，纯逻辑验证
// ================================================================
class MockActionTool : public AITool {
public:
    MockActionTool()
        : AITool("mock_action", "A mock action tool for testing", ToolCategory::Action) {}

    QJsonObject parameterSchema() const override {
        QJsonObject nameProp;
        nameProp["type"] = "string";
        nameProp["description"] = "Name parameter";

        QJsonObject countProp;
        countProp["type"] = "integer";
        countProp["description"] = "Count parameter";

        QJsonObject properties;
        properties["name"] = nameProp;
        properties["count"] = countProp;

        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = properties;
        schema["required"] = QJsonArray{"name"};

        return schema;
    }

    ToolResult execute(const QJsonObject& params) override {
        QString name = params["name"].toString();
        int count = params.value("count").toInt(1);

        QJsonObject result;
        result["echo_name"] = name;
        result["echo_count"] = count;
        result["executed"] = true;

        return ToolResult::ok(result);
    }
};

class MockQueryTool : public AITool {
public:
    MockQueryTool()
        : AITool("mock_query", "A mock query tool for testing", ToolCategory::Query) {}

    QJsonObject parameterSchema() const override {
        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = QJsonObject{};
        return schema;
    }

    ToolResult execute(const QJsonObject& /*params*/) override {
        QJsonObject result;
        result["value"] = 42;
        result["message"] = "query ok";
        return ToolResult::ok(result);
    }
};

class MockSensitiveQueryTool : public AITool {
public:
    MockSensitiveQueryTool()
        : AITool("mock_sensitive_query", "A query tool returning sensitive fields", ToolCategory::Query) {}

    QJsonObject parameterSchema() const override {
        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = QJsonObject{};
        return schema;
    }

    ToolResult execute(const QJsonObject& /*params*/) override {
        QJsonObject result;
        result["api_key"] = "should-not-leak";
        result["message"] = "safe";
        return ToolResult::ok(result);
    }
};

class MockLargeQueryTool : public AITool {
public:
    MockLargeQueryTool()
        : AITool("mock_large_query", "A query tool returning a large payload", ToolCategory::Query) {}

    QJsonObject parameterSchema() const override {
        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = QJsonObject{};
        return schema;
    }

    ToolResult execute(const QJsonObject& /*params*/) override {
        QJsonObject result;
        result["blob"] = QString(4096, QLatin1Char('x'));
        return ToolResult::ok(result);
    }
};

class MockShellTool : public AITool {
public:
    MockShellTool()
        : AITool("shell_execute", "A high risk shell tool", ToolCategory::Action) {}

    QJsonObject parameterSchema() const override {
        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = QJsonObject{};
        return schema;
    }

    ToolResult execute(const QJsonObject& /*params*/) override {
        return ToolResult::ok(QJsonObject{{"executed", true}});
    }
};

class MockDeleteTool : public AITool {
public:
    MockDeleteTool()
        : AITool("delete_file", "A dangerous delete tool", ToolCategory::Action) {}

    QJsonObject parameterSchema() const override {
        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = QJsonObject{};
        return schema;
    }

    ToolResult execute(const QJsonObject& /*params*/) override {
        return ToolResult::ok(QJsonObject{{"deleted", true}});
    }
};

class MockWhitelistedCommandTool : public AITool {
public:
    MockWhitelistedCommandTool()
        : AITool("execute_whitelisted_command", "A scoped command tool", ToolCategory::Action) {}

    QJsonObject parameterSchema() const override {
        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = QJsonObject{};
        return schema;
    }

    ToolResult execute(const QJsonObject& /*params*/) override {
        return ToolResult::ok(QJsonObject{{"executed", true}});
    }
};

// ================================================================
// 测试类
// ================================================================
class TestToolRegistry : public QObject {
    Q_OBJECT

private slots:
    // --- ToolResult 测试 ---
    void testToolResultOk();
    void testToolResultFail();
    void testToolResultToJson();

    // --- AITool 基类测试 ---
    void testToolBasicProperties();
    void testToolFunctionSchema();
    void testToolValidation();
    void testToolValidationMissingRequired();

    // --- ToolRegistry 测试 ---
    void testRegistryEmpty();
    void testRegisterAndFind();
    void testRegisterMultipleTools();
    void testFindNonExistent();
    void testExecuteTool();
    void testExecuteUnknownTool();
    void testExecuteWithInvalidParams();
    void testAllToolSchemas();

    // --- ToolRuntime / PolicyEngine 测试 ---
    void testPolicyAllowsSafeQuery();
    void testRuntimeBlocksUnknownTool();
    void testRuntimeRequiresConfirmationForHighRiskTool();
    void testRuntimeCancelsConfirmationWhenRegistryChanges();
    void testRuntimeKeepsDuplicateConfirmationIdsDistinct();
    void testPolicyDoesNotAutoGrantHighRiskTool();
    void testRuntimeDeniesDangerousTool();
    void testRuntimeSanitizesSensitiveOutput();
    void testRuntimeTruncatesLargePayload();
    void testPolicyDeniesScopedToolOutsideAllowedRoots();
    void testSchedulerRollsBackWhenSaveFails();

    // --- 真实 Tool 测试 ---
    void testGetCurrentTimeTool();
};

// ============================================================
// ToolResult 测试
// ============================================================

void TestToolRegistry::testToolResultOk() {
    QJsonObject data;
    data["key"] = "value";
    ToolResult result = ToolResult::ok(data);

    QVERIFY(result.success);
    QCOMPARE(result.data["key"].toString(), QString("value"));
    QVERIFY(result.errorMessage.isEmpty());
}

void TestToolRegistry::testToolResultFail() {
    ToolResult result = ToolResult::fail("something went wrong");

    QVERIFY(!result.success);
    QCOMPARE(result.errorMessage, QString("something went wrong"));
}

void TestToolRegistry::testToolResultToJson() {
    // 成功时应包含 result 字段
    ToolResult ok = ToolResult::ok(QJsonObject{{"x", 1}});
    QJsonObject okJson = ok.toJson();
    QVERIFY(okJson["success"].toBool());
    QVERIFY(okJson.contains("data"));
    QVERIFY(!okJson.contains("error"));

    // 失败时应包含 error 字段
    ToolResult fail = ToolResult::fail("oops");
    QJsonObject failJson = fail.toJson();
    QVERIFY(!failJson["success"].toBool());
    QVERIFY(failJson.contains("error"));
    QCOMPARE(failJson["error"].toString(), QString("oops"));
}

// ============================================================
// AITool 基类测试
// ============================================================

void TestToolRegistry::testToolBasicProperties() {
    MockActionTool tool;
    QCOMPARE(tool.name(), QString("mock_action"));
    QCOMPARE(tool.description(), QString("A mock action tool for testing"));
    QCOMPARE(tool.category(), ToolCategory::Action);

    MockQueryTool query;
    QCOMPARE(query.category(), ToolCategory::Query);
}

void TestToolRegistry::testToolFunctionSchema() {
    MockActionTool tool;
    QJsonObject schema = tool.toFunctionSchema();

    // 验证顶层结构
    QCOMPARE(schema["type"].toString(), QString("function"));
    QVERIFY(schema.contains("function"));

    // 验证 function 内部
    QJsonObject func = schema["function"].toObject();
    QCOMPARE(func["name"].toString(), QString("mock_action"));
    QVERIFY(func.contains("description"));
    QVERIFY(func.contains("parameters"));

    // 验证 parameters
    QJsonObject params = func["parameters"].toObject();
    QCOMPARE(params["type"].toString(), QString("object"));
    QVERIFY(params.contains("properties"));
    QVERIFY(params.contains("required"));

    // 打印完整 Schema 供目视检查
    qDebug() << "Function Schema:"
             << QJsonDocument(schema).toJson(QJsonDocument::Indented);
}

void TestToolRegistry::testToolValidation() {
    MockActionTool tool;

    // 包含 required 字段 → 通过
    QJsonObject validParams;
    validParams["name"] = "test";
    QVERIFY(tool.validate(validParams));

    // 包含所有字段 → 通过
    validParams["count"] = 5;
    QVERIFY(tool.validate(validParams));
}

void TestToolRegistry::testToolValidationMissingRequired() {
    MockActionTool tool;

    // 缺少 required 字段 "name" → 失败
    QJsonObject invalidParams;
    invalidParams["count"] = 5;
    QVERIFY(!tool.validate(invalidParams));

    // 空参数 → 失败
    QVERIFY(!tool.validate(QJsonObject{}));
}

// ============================================================
// ToolRegistry 测试
// ============================================================

void TestToolRegistry::testRegistryEmpty() {
    ToolRegistry registry;
    QCOMPARE(registry.toolCount(), 0);
    QVERIFY(registry.toolNames().isEmpty());
    QVERIFY(!registry.hasTool("anything"));
}

void TestToolRegistry::testRegisterAndFind() {
    ToolRegistry registry;
    registry.registerTool(std::make_unique<MockActionTool>());

    QCOMPARE(registry.toolCount(), 1);
    QVERIFY(registry.hasTool("mock_action"));

    AITool* found = registry.getTool("mock_action");
    QVERIFY(found != nullptr);
    QCOMPARE(found->name(), QString("mock_action"));
}

void TestToolRegistry::testRegisterMultipleTools() {
    ToolRegistry registry;
    registry.registerTool(std::make_unique<MockActionTool>());
    registry.registerTool(std::make_unique<MockQueryTool>());

    QCOMPARE(registry.toolCount(), 2);
    QVERIFY(registry.hasTool("mock_action"));
    QVERIFY(registry.hasTool("mock_query"));

    QStringList names = registry.toolNames();
    QVERIFY(names.contains("mock_action"));
    QVERIFY(names.contains("mock_query"));
}

void TestToolRegistry::testFindNonExistent() {
    ToolRegistry registry;
    QVERIFY(registry.getTool("does_not_exist") == nullptr);
}

void TestToolRegistry::testExecuteTool() {
    ToolRegistry registry;
    registry.registerTool(std::make_unique<MockActionTool>());

    // 正常执行
    QJsonObject params;
    params["name"] = "hello";
    params["count"] = 3;

    ToolResult result = registry.executeTool("mock_action", params);

    QVERIFY(result.success);
    QCOMPARE(result.data["echo_name"].toString(), QString("hello"));
    QCOMPARE(result.data["echo_count"].toInt(), 3);
    QVERIFY(result.data["executed"].toBool());
}

void TestToolRegistry::testExecuteUnknownTool() {
    ToolRegistry registry;
    ToolResult result = registry.executeTool("nonexistent", QJsonObject{});

    QVERIFY(!result.success);
    QVERIFY(result.errorMessage.contains("Unknown tool"));
}

void TestToolRegistry::testExecuteWithInvalidParams() {
    ToolRegistry registry;
    registry.registerTool(std::make_unique<MockActionTool>());

    // 缺少 required 字段
    ToolResult result = registry.executeTool("mock_action", QJsonObject{});

    QVERIFY(!result.success);
    QVERIFY(result.errorMessage.contains("missing required fields"));
}

void TestToolRegistry::testAllToolSchemas() {
    ToolRegistry registry;
    registry.registerTool(std::make_unique<MockActionTool>());
    registry.registerTool(std::make_unique<MockQueryTool>());

    QJsonArray schemas = registry.allToolSchemas();
    QCOMPARE(schemas.size(), 2);

    // 每个 schema 都应该有 type=function 和 function 对象
    for (const auto& s : schemas) {
        QJsonObject schema = s.toObject();
        QCOMPARE(schema["type"].toString(), QString("function"));
        QVERIFY(schema.contains("function"));
    }

    // 打印完整 schemas 供目视检查（这就是将来发给 LLM 的 tools 参数）
    qDebug() << "All Tool Schemas:"
             << QJsonDocument(schemas).toJson(QJsonDocument::Indented);
}

// ============================================================
// ToolRuntime / PolicyEngine 测试
// ============================================================

void TestToolRegistry::testPolicyAllowsSafeQuery() {
    MockQueryTool tool;
    PolicyEngine policy;
    const ToolPolicyDecision decision = policy.evaluate(tool, QJsonObject{}, ToolPolicyContext{});

    QVERIFY(decision.isAllowed());
    QCOMPARE(decision.riskLevel, ToolRiskLevel::L0SafeRead);
}

void TestToolRegistry::testRuntimeBlocksUnknownTool() {
    ToolRegistry registry;
    ToolRuntime runtime;
    runtime.setToolRegistry(&registry);

    ToolExecutionRequest request;
    request.toolName = "missing_tool";

    const ToolExecutionOutcome outcome = runtime.execute(request);
    QVERIFY(!outcome.executed);
    QVERIFY(!outcome.result.success);
    QVERIFY(outcome.policyDecision.isDenied());
}

void TestToolRegistry::testRuntimeRequiresConfirmationForHighRiskTool() {
    ToolRegistry registry;
    registry.registerTool(std::make_unique<MockShellTool>());

    ToolRuntime runtime;
    runtime.setToolRegistry(&registry);

    ToolExecutionRequest request;
    request.toolName = "shell_execute";

    const ToolExecutionOutcome outcome = runtime.execute(request);
    QVERIFY(!outcome.executed);
    QVERIFY(!outcome.result.success);
    QVERIFY(outcome.policyDecision.needsConfirmation());
    QCOMPARE(outcome.policyDecision.riskLevel, ToolRiskLevel::L3HighRiskAction);
    QVERIFY(runtime.hasPendingConfirmation(outcome.requestId));
}

void TestToolRegistry::testRuntimeCancelsConfirmationWhenRegistryChanges() {
    ToolRegistry originalRegistry;
    originalRegistry.registerTool(std::make_unique<MockShellTool>());
    ToolRegistry replacementRegistry;
    replacementRegistry.registerTool(std::make_unique<MockShellTool>());

    ToolRuntime runtime;
    runtime.setToolRegistry(&originalRegistry);

    ToolExecutionRequest request;
    request.toolName = "shell_execute";
    const ToolExecutionOutcome pending = runtime.execute(request);
    QVERIFY(runtime.hasPendingConfirmation(pending.requestId));

    runtime.setToolRegistry(&replacementRegistry);
    QVERIFY(!runtime.hasPendingConfirmation(pending.requestId));
    const ToolExecutionOutcome resolved = runtime.resolveConfirmation(pending.requestId, true);
    QVERIFY(!resolved.executed);
    QVERIFY(resolved.policyDecision.isDenied());
}

void TestToolRegistry::testRuntimeKeepsDuplicateConfirmationIdsDistinct() {
    ToolRegistry registry;
    registry.registerTool(std::make_unique<MockShellTool>());
    ToolRuntime runtime;
    runtime.setToolRegistry(&registry);

    ToolExecutionRequest request;
    request.requestId = QStringLiteral("duplicate-id");
    request.toolName = QStringLiteral("shell_execute");
    const ToolExecutionOutcome first = runtime.execute(request);
    const ToolExecutionOutcome second = runtime.execute(request);

    QCOMPARE(first.requestId, QStringLiteral("duplicate-id"));
    QVERIFY(first.requestId != second.requestId);
    QVERIFY(runtime.hasPendingConfirmation(first.requestId));
    QVERIFY(runtime.hasPendingConfirmation(second.requestId));
}

void TestToolRegistry::testPolicyDoesNotAutoGrantHighRiskTool() {
    MockShellTool tool;
    PolicyEngine policy;
    ToolPolicyContext context;
    context.grantedToolNames = {"shell_execute"};

    const ToolPolicyDecision decision = policy.evaluate(tool, {}, context);
    QVERIFY(decision.needsConfirmation());
    QCOMPARE(decision.riskLevel, ToolRiskLevel::L3HighRiskAction);
}

void TestToolRegistry::testRuntimeDeniesDangerousTool() {
    ToolRegistry registry;
    registry.registerTool(std::make_unique<MockDeleteTool>());

    ToolRuntime runtime;
    runtime.setToolRegistry(&registry);

    ToolExecutionRequest request;
    request.toolName = "delete_file";

    const ToolExecutionOutcome outcome = runtime.execute(request);
    QVERIFY(!outcome.executed);
    QVERIFY(!outcome.result.success);
    QVERIFY(outcome.policyDecision.isDenied());
    QCOMPARE(outcome.policyDecision.riskLevel, ToolRiskLevel::L4Dangerous);
}

void TestToolRegistry::testRuntimeSanitizesSensitiveOutput() {
    ToolRegistry registry;
    registry.registerTool(std::make_unique<MockSensitiveQueryTool>());

    ToolRuntime runtime;
    runtime.setToolRegistry(&registry);

    ToolExecutionRequest request;
    request.toolName = "mock_sensitive_query";

    const ToolExecutionOutcome outcome = runtime.execute(request);
    QVERIFY(outcome.executed);
    QVERIFY(outcome.result.success);
    QCOMPARE(outcome.result.data.value("api_key").toString(), QString("[REDACTED]"));
    QCOMPARE(outcome.result.data.value("message").toString(), QString("safe"));
}

void TestToolRegistry::testRuntimeTruncatesLargePayload() {
    ToolRegistry registry;
    registry.registerTool(std::make_unique<MockLargeQueryTool>());

    ToolRuntime runtime;
    runtime.setToolRegistry(&registry);
    runtime.sanitizer()->setMaxPayloadLength(512);

    ToolExecutionRequest request;
    request.toolName = "mock_large_query";

    const ToolExecutionOutcome outcome = runtime.execute(request);
    QVERIFY(outcome.executed);
    QVERIFY(outcome.result.success);

    const QString payload = runtime.sanitizer()->toPayload(outcome.result);
    QVERIFY(payload.size() <= 512);

    const QJsonObject payloadJson = QJsonDocument::fromJson(payload.toUtf8()).object();
    QVERIFY(payloadJson.value("success").toBool());
    QVERIFY(payloadJson.value("data").toObject().value("truncated").toBool());
}

void TestToolRegistry::testPolicyDeniesScopedToolOutsideAllowedRoots() {
    MockWhitelistedCommandTool tool;
    PolicyEngine policy;

    ToolPolicyContext context;
    context.allowedRootPaths = {"C:/Allowed"};

    QJsonObject args;
    args["working_directory"] = "C:/Windows";

    const ToolPolicyDecision decision = policy.evaluate(tool, args, context);
    QVERIFY(decision.isDenied());
}

void TestToolRegistry::testSchedulerRollsBackWhenSaveFails() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString blockingPath = dir.filePath("not-a-directory");
    QFile blocker(blockingPath);
    QVERIFY(blocker.open(QIODevice::WriteOnly));
    blocker.write("block");
    blocker.close();

    AgentScheduler scheduler;
    scheduler.setStoragePath(QDir(blockingPath).filePath("tasks.json"));
    QJsonObject params{
        {"title", "persist me"},
        {"message", "message"},
        {"type", "once_at"},
        {"delay_minutes", 5}
    };
    QString error;
    const ScheduledTask task = scheduler.createTask(params, &error);
    QVERIFY(task.id.isEmpty());
    QVERIFY(!error.isEmpty());
    QVERIFY(scheduler.tasks().isEmpty());
}

// ============================================================
// 真实 Tool 测试
// ============================================================

void TestToolRegistry::testGetCurrentTimeTool() {
    ToolRegistry registry;
    registry.registerTool(std::make_unique<GetCurrentTimeTool>());

    // 执行
    ToolResult result = registry.executeTool("get_current_time", QJsonObject{});

    QVERIFY(result.success);
    QVERIFY(result.data.contains("datetime"));
    QVERIFY(result.data.contains("time"));
    QVERIFY(result.data.contains("hour"));
    QVERIFY(result.data.contains("period"));

    // hour 应该是 0-23
    int hour = result.data["hour"].toInt();
    QVERIFY(hour >= 0 && hour <= 23);

    // period 应该是已知的中文时段
    QString period = result.data["period"].toString();
    QStringList validPeriods = {"凌晨", "上午", "中午", "下午", "晚上", "深夜"};
    QVERIFY2(validPeriods.contains(period),
             qPrintable(QString("Unexpected period: %1").arg(period)));

    qDebug() << "Time tool result:"
             << QJsonDocument(result.data).toJson(QJsonDocument::Indented);
}

QTEST_MAIN(TestToolRegistry)
#include "test_tool_registry.moc"
