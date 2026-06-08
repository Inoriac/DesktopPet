// ================================================================
// 文件和网络工具测试
// 注意: 网络测试需要实际网络连接，可能不稳定
// ================================================================

#include <QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <QJsonObject>
#include <QJsonArray>

#include "ai/tools/file_tools.h"
#include "ai/tools/web_tools.h"

class TestFileAndWebTools : public QObject {
    Q_OBJECT

private slots:
    // 文件工具测试
    void test_read_text_file_success();
    void test_read_text_file_not_found();
    void test_read_text_file_path_traversal();
    void test_list_directory_success();
    void test_list_directory_not_found();

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
    QVERIFY(result.errorMessage.contains("不允许") || result.errorMessage.contains("not allowed"));
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

    // 注意: example.com 等需要 DNS 解析的测试在网络不可用时会失败
    // 因此不在这里测试，实际使用时会正常工作
    qDebug() << "Network security validator basic tests passed";
}

QTEST_MAIN(TestFileAndWebTools)
#include "test_file_and_web_tools.moc"