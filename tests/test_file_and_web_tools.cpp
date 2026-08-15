// ================================================================
// 文件和网络工具测试
// 注意: 网络测试需要实际网络连接，可能不稳定
// ================================================================

#include <QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QJsonObject>
#include <QJsonArray>
#include <filesystem>

#include "ai/tool_registry.h"
#include "ai/tools/file_tools.h"
#include "ai/tools/runtime/tool_runtime.h"
#include "ai/tools/web_tools.h"

class TestFileAndWebTools : public QObject {
    Q_OBJECT

private slots:
    // 文件工具测试
    void test_read_text_file_success();
    void test_read_text_file_not_found();
    void test_read_text_file_path_traversal();
    void test_file_tools_reject_symlink_escape();
    void test_file_path_validator_keeps_root_pairs_aligned();
    void test_list_directory_success();
    void test_list_directory_not_found();
    void test_write_text_file_success();
    void test_write_text_file_no_overwrite_by_default();
    void test_write_text_file_overwrite_when_explicit();
    void test_write_text_file_rejects_outside_root();
    void test_write_text_file_rejects_oversized_content();
    void test_write_text_file_blocks_sensitive_target();
    void test_execute_whitelisted_command_success();
    void test_execute_whitelisted_command_rejects_unlisted();
    void test_execute_whitelisted_command_rejects_shell();
    void test_execute_whitelisted_command_rejects_shell_tokens();
    void test_execute_whitelisted_command_rejects_outside_path_arg();
    void test_tool_runtime_requires_confirmation_for_write();
    void test_tool_runtime_executes_confirmed_write();
    void test_tool_runtime_rejects_denied_write();

    // 网络工具测试 (验证参数和本地逻辑)
    void test_web_fetch_validate_params();
    void test_web_fetch_block_localhost();
    void test_web_search_validate_params();
    void test_network_security_validator();
};

void TestFileAndWebTools::test_read_text_file_success() {
    // 创建临时文件
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    QString testFilePath = tempDir.filePath("test.txt");
    QFile testFile(testFilePath);
    QVERIFY(testFile.open(QIODevice::WriteOnly));
    testFile.write("Hello World\nLine 2\nLine 3");
    testFile.close();

    // 设置允许访问临时目录
    QStringList allowedRoots;
    allowedRoots.append(tempDir.path());

    ReadTextFileTool tool(allowedRoots);

    QJsonObject params;
    params["path"] = testFilePath;

    ToolResult result = tool.execute(params);

    QBENCHMARK {
        result = tool.execute(params);
    }

    QVERIFY(result.success);
    QVERIFY(result.data.contains("content"));
    QVERIFY(result.data.value("content").toString().contains("Hello World"));
}

void TestFileAndWebTools::test_read_text_file_not_found() {
    QStringList allowedRoots;
    allowedRoots.append(QCoreApplication::applicationDirPath());

    ReadTextFileTool tool(allowedRoots);

    QJsonObject params;
    params["path"] = "/nonexistent/path/to/file.txt";

    ToolResult result = tool.execute(params);

    // 应该失败，不检查具体错误消息内容
    QVERIFY(!result.success);
    qDebug() << "Error message:" << result.errorMessage;
}

void TestFileAndWebTools::test_read_text_file_path_traversal() {
    // 测试路径穿越防护
    QStringList allowedRoots;
    allowedRoots.append("C:/Windows/System32");  // 故意设置一个不允许的路径

    ReadTextFileTool tool(allowedRoots);

    QJsonObject params;
    params["path"] = "C:/Windows/System32/../../Windows/win.ini";

    ToolResult result = tool.execute(params);

    // 应该被拒绝
    QVERIFY(!result.success);
}

void TestFileAndWebTools::test_file_tools_reject_symlink_escape() {
    QTemporaryDir allowedDir;
    QTemporaryDir outsideDir;
    QVERIFY(allowedDir.isValid());
    QVERIFY(outsideDir.isValid());

    QFile secret(outsideDir.filePath("secret.txt"));
    QVERIFY(secret.open(QIODevice::WriteOnly));
    secret.write("outside");
    secret.close();

    const QString linkPath = allowedDir.filePath("escape");
    std::error_code error;
    std::filesystem::create_directory_symlink(
        std::filesystem::u8path(outsideDir.path().toUtf8().constData()),
        std::filesystem::u8path(linkPath.toUtf8().constData()),
        error);
    if (error) {
        QSKIP(qPrintable(QString("Cannot create directory symlink in this environment: %1")
                             .arg(QString::fromStdString(error.message()))));
    }

    ReadTextFileTool readTool({allowedDir.path()});
    QJsonObject readParams{{"path", QDir(linkPath).filePath("secret.txt")}};
    QVERIFY(!readTool.execute(readParams).success);

    WriteTextFileTool writeTool({allowedDir.path()}, 1024);
    QJsonObject writeParams{
        {"path", QDir(linkPath).filePath("created.txt")},
        {"content", "must not escape"}
    };
    QVERIFY(!writeTool.execute(writeParams).success);
}

void TestFileAndWebTools::test_file_path_validator_keeps_root_pairs_aligned() {
    QTemporaryDir allowedDir;
    QVERIFY(allowedDir.isValid());

    const QString missingRoot = allowedDir.filePath("missing-root");
    const QString filePath = allowedDir.filePath("allowed.txt");
    FilePathValidator validator({missingRoot, allowedDir.path()});

    QCOMPARE(QDir::cleanPath(validator.getAllowedRoot(filePath)),
             QDir::cleanPath(allowedDir.path()));
}

void TestFileAndWebTools::test_list_directory_success() {
    // 创建临时目录结构
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString dirPath = tempDir.path();

    // 创建文件和子目录
    QFile file1(dirPath + "/file1.txt");
    QFile file2(dirPath + "/file2.txt");
    QDir(dirPath).mkdir("subdir");

    QVERIFY(file1.open(QIODevice::WriteOnly));
    file1.write("test");
    file1.close();

    QVERIFY(file2.open(QIODevice::WriteOnly));
    file2.write("test");
    file2.close();

    // 设置允许访问临时目录
    QStringList allowedRoots;
    allowedRoots.append(dirPath);

    ListDirectoryTool tool(allowedRoots);

    QJsonObject params;
    params["path"] = dirPath;

    ToolResult result = tool.execute(params);

    qDebug() << "List directory result:" << result.success << result.errorMessage;

    QVERIFY(result.success);
    QVERIFY(result.data.contains("entries"));
    int count = result.data.value("count").toInt();
    qDebug() << "Entry count:" << count;
    QVERIFY(count >= 2);  // 至少 2 个文件
}

void TestFileAndWebTools::test_list_directory_not_found() {
    QStringList allowedRoots;
    allowedRoots.append(QCoreApplication::applicationDirPath());

    ListDirectoryTool tool(allowedRoots);

    QJsonObject params;
    params["path"] = "/nonexistent/directory";

    ToolResult result = tool.execute(params);

    QVERIFY(!result.success);
}

void TestFileAndWebTools::test_write_text_file_success() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    WriteTextFileTool tool(QStringList{tempDir.path()}, 1024);

    const QString filePath = tempDir.filePath("notes/output.txt");
    QJsonObject params;
    params["path"] = filePath;
    params["content"] = "hello safe write";

    const ToolResult result = tool.execute(params);
    QVERIFY(result.success);
    QVERIFY(QFileInfo::exists(filePath));

    QFile written(filePath);
    QVERIFY(written.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(QString::fromUtf8(written.readAll()), QString("hello safe write"));
}

void TestFileAndWebTools::test_write_text_file_no_overwrite_by_default() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString filePath = tempDir.filePath("existing.txt");
    QFile existing(filePath);
    QVERIFY(existing.open(QIODevice::WriteOnly | QIODevice::Text));
    existing.write("original");
    existing.close();

    WriteTextFileTool tool(QStringList{tempDir.path()}, 1024);

    QJsonObject params;
    params["path"] = filePath;
    params["content"] = "changed";

    const ToolResult result = tool.execute(params);
    QVERIFY(!result.success);

    QFile check(filePath);
    QVERIFY(check.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(QString::fromUtf8(check.readAll()), QString("original"));
}

void TestFileAndWebTools::test_write_text_file_overwrite_when_explicit() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString filePath = tempDir.filePath("existing.txt");
    QFile existing(filePath);
    QVERIFY(existing.open(QIODevice::WriteOnly | QIODevice::Text));
    existing.write("original");
    existing.close();

    WriteTextFileTool tool(QStringList{tempDir.path()}, 1024);

    QJsonObject params;
    params["path"] = filePath;
    params["content"] = "changed";
    params["overwrite"] = true;

    const ToolResult result = tool.execute(params);
    QVERIFY(result.success);

    QFile check(filePath);
    QVERIFY(check.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(QString::fromUtf8(check.readAll()), QString("changed"));
}

void TestFileAndWebTools::test_write_text_file_rejects_outside_root() {
    QTemporaryDir allowedDir;
    QTemporaryDir outsideDir;
    QVERIFY(allowedDir.isValid());
    QVERIFY(outsideDir.isValid());

    WriteTextFileTool tool(QStringList{allowedDir.path()}, 1024);

    const QString outsidePath = outsideDir.filePath("outside.txt");
    QJsonObject params;
    params["path"] = outsidePath;
    params["content"] = "nope";

    const ToolResult result = tool.execute(params);
    QVERIFY(!result.success);
    QVERIFY(!QFileInfo::exists(outsidePath));
}

void TestFileAndWebTools::test_write_text_file_rejects_oversized_content() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    WriteTextFileTool tool(QStringList{tempDir.path()}, 8);

    QJsonObject params;
    params["path"] = tempDir.filePath("large.txt");
    params["content"] = "this content is too large";

    const ToolResult result = tool.execute(params);
    QVERIFY(!result.success);
}

void TestFileAndWebTools::test_write_text_file_blocks_sensitive_target() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    WriteTextFileTool tool(QStringList{tempDir.path()}, 1024);

    QJsonObject params;
    params["path"] = tempDir.filePath(".env");
    params["content"] = "TOKEN=bad";

    const ToolResult result = tool.execute(params);
    QVERIFY(!result.success);
}

void TestFileAndWebTools::test_execute_whitelisted_command_success() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    CommandExecutionPolicy policy;
    policy.allowedRoots = {tempDir.path()};
    policy.commandWhitelist = {QCoreApplication::applicationFilePath()};
    policy.timeoutMs = 5000;

    ExecuteWhitelistedCommandTool tool(policy);
    QJsonObject params;
    params["working_directory"] = tempDir.path();
    params["command"] = QCoreApplication::applicationFilePath();
    params["args"] = QJsonArray{"test_execute_whitelisted_command_rejects_shell"};

    const ToolResult result = tool.execute(params);
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QCOMPARE(result.data.value("exit_code").toInt(-1), 0);
}

void TestFileAndWebTools::test_execute_whitelisted_command_rejects_unlisted() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    CommandExecutionPolicy policy;
    policy.allowedRoots = {tempDir.path()};
    policy.commandWhitelist = {"some-other-command"};

    ExecuteWhitelistedCommandTool tool(policy);
    QJsonObject params;
    params["working_directory"] = tempDir.path();
    params["command"] = QCoreApplication::applicationFilePath();

    const ToolResult result = tool.execute(params);
    QVERIFY(!result.success);
}

void TestFileAndWebTools::test_execute_whitelisted_command_rejects_shell() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    CommandExecutionPolicy policy;
    policy.allowedRoots = {tempDir.path()};
    policy.commandWhitelist = {"cmd.exe"};

    ExecuteWhitelistedCommandTool tool(policy);
    QJsonObject params;
    params["working_directory"] = tempDir.path();
    params["command"] = "cmd.exe";

    const ToolResult result = tool.execute(params);
    QVERIFY(!result.success);
}

void TestFileAndWebTools::test_execute_whitelisted_command_rejects_shell_tokens() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    CommandExecutionPolicy policy;
    policy.allowedRoots = {tempDir.path()};
    policy.commandWhitelist = {QCoreApplication::applicationFilePath()};

    ExecuteWhitelistedCommandTool tool(policy);
    QJsonObject params;
    params["working_directory"] = tempDir.path();
    params["command"] = QCoreApplication::applicationFilePath();
    params["args"] = QJsonArray{"safe", "&&", "unsafe"};

    const ToolResult result = tool.execute(params);
    QVERIFY(!result.success);
}

void TestFileAndWebTools::test_execute_whitelisted_command_rejects_outside_path_arg() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    CommandExecutionPolicy policy;
    policy.allowedRoots = {tempDir.path()};
    policy.commandWhitelist = {"git"};

    ExecuteWhitelistedCommandTool tool(policy);
    QJsonObject params;
    params["working_directory"] = tempDir.path();
    params["command"] = "git";
    params["args"] = QJsonArray{"status", "C:/Windows"};

    const ToolResult result = tool.execute(params);
    QVERIFY(!result.success);
}

void TestFileAndWebTools::test_tool_runtime_requires_confirmation_for_write() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    ToolRegistry registry;
    registry.registerTool(std::make_unique<WriteTextFileTool>(QStringList{tempDir.path()}, 1024));

    ToolRuntime runtime;
    runtime.setToolRegistry(&registry);

    ToolExecutionRequest request;
    request.toolName = "write_text_file";
    request.policyContext.allowedRootPaths = {tempDir.path()};
    request.arguments["path"] = tempDir.filePath("runtime.txt");
    request.arguments["content"] = "runtime write";

    const ToolExecutionOutcome outcome = runtime.execute(request);
    QVERIFY(!outcome.executed);
    QVERIFY(outcome.policyDecision.needsConfirmation());
    QVERIFY(!QFileInfo::exists(tempDir.filePath("runtime.txt")));
}

void TestFileAndWebTools::test_tool_runtime_executes_confirmed_write() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    ToolRegistry registry;
    registry.registerTool(std::make_unique<WriteTextFileTool>(QStringList{tempDir.path()}, 1024));

    ToolRuntime runtime;
    runtime.setToolRegistry(&registry);

    const QString filePath = tempDir.filePath("runtime.txt");
    ToolExecutionRequest request;
    request.toolName = "write_text_file";
    request.policyContext.allowedRootPaths = {tempDir.path()};
    request.arguments["path"] = filePath;
    request.arguments["content"] = "runtime write";
    const ToolExecutionOutcome pending = runtime.execute(request);
    QVERIFY(pending.policyDecision.needsConfirmation());
    QVERIFY(runtime.hasPendingConfirmation(pending.requestId));

    const ToolExecutionOutcome outcome = runtime.resolveConfirmation(pending.requestId, true);
    QVERIFY2(outcome.executed, qPrintable(outcome.policyDecision.reason));
    QVERIFY2(outcome.result.success, qPrintable(outcome.result.errorMessage));
    QVERIFY(QFileInfo::exists(filePath));
}

void TestFileAndWebTools::test_tool_runtime_rejects_denied_write() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    ToolRegistry registry;
    registry.registerTool(std::make_unique<WriteTextFileTool>(QStringList{tempDir.path()}, 1024));
    ToolRuntime runtime;
    runtime.setToolRegistry(&registry);

    ToolExecutionRequest request;
    request.requestId = "deny-write";
    request.toolName = "write_text_file";
    request.policyContext.allowedRootPaths = {tempDir.path()};
    request.arguments["path"] = tempDir.filePath("runtime.txt");
    request.arguments["content"] = "runtime write";

    const ToolExecutionOutcome pending = runtime.execute(request);
    QVERIFY(runtime.hasPendingConfirmation(pending.requestId));
    const ToolExecutionOutcome rejected = runtime.resolveConfirmation(pending.requestId, false);
    QVERIFY(!rejected.executed);
    QVERIFY(rejected.policyDecision.isDenied());
    QVERIFY(!runtime.hasPendingConfirmation(pending.requestId));
    QVERIFY(!QFileInfo::exists(tempDir.filePath("runtime.txt")));
}

void TestFileAndWebTools::test_web_fetch_validate_params() {
    WebFetchTool tool;

    // 测试无效 URL - 空参数
    QJsonObject params1;
    params1["url"] = "";

    ToolResult result1 = tool.execute(params1);
    QVERIFY(!result1.success);

    // 测试非 HTTP/HTTPS URL
    QJsonObject params2;
    params2["url"] = "ftp://example.com/file.txt";

    ToolResult result2 = tool.execute(params2);
    QVERIFY(!result2.success);
}

void TestFileAndWebTools::test_web_fetch_block_localhost() {
    WebFetchTool tool;

    QJsonObject params;
    params["url"] = "http://localhost/test";

    ToolResult result = tool.execute(params);
    QVERIFY(!result.success);
    QVERIFY(result.errorMessage.contains("不允许")
            || result.errorMessage.contains("受限")
            || result.errorMessage.contains("not allowed"));
}

void TestFileAndWebTools::test_web_search_validate_params() {
    WebSearchTool tool;

    // 测试空查询
    QJsonObject params1;
    params1["query"] = "";

    ToolResult result1 = tool.execute(params1);
    QVERIFY(!result1.success);
    QVERIFY(result1.errorMessage.contains("关键词") || result1.errorMessage.contains("empty"));
}

void TestFileAndWebTools::test_network_security_validator() {
    NetworkSecurityValidator validator;

    // 测试无效 scheme - 这些不需要 DNS 解析
    QVERIFY(!validator.isUrlAllowed("ftp://example.com"));
    QVERIFY(!validator.isUrlAllowed("file:///path"));

    // 测试 localhost - 不需要 DNS 解析
    QVERIFY(!validator.isUrlAllowed("http://localhost/test"));
    QVERIFY(!validator.isUrlAllowed("http://127.0.0.1/test"));

    // 测试内网 IP - 不需要 DNS 解析
    QVERIFY(!validator.isUrlAllowed("http://192.168.1.1/"));
    QVERIFY(!validator.isUrlAllowed("http://10.0.0.1/"));
    QVERIFY(!validator.isUrlAllowed("http://172.16.0.1/"));
    QVERIFY(!validator.isUrlAllowed("http://[0:0:0:0:0:0:0:1]/"));
    QVERIFY(!validator.isUrlAllowed("http://[fc00::1]/"));
    QVERIFY(!validator.isUrlAllowed("http://[fe80::1]/"));
    QVERIFY(validator.isUrlAllowed("http://[2606:4700:4700::1111]/"));

    // 注意: example.com 等需要 DNS 解析的测试在网络不可用时会失败
    // 因此不在这里测试，实际使用时会正常工作
    qDebug() << "Network security validator basic tests passed";
}

QTEST_MAIN(TestFileAndWebTools)
#include "test_file_and_web_tools.moc"
