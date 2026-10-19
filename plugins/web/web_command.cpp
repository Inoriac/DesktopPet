#include "web_command.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <cmath>

namespace {
constexpr int MaxInputBytes = 16 * 1024;

QJsonObject failed(const QString& code, const QString& message) {
    const QJsonObject error{{"code", code}, {"message", message}};
    return {{"state", "failed"}, {"data", QJsonObject{{"error", error}}}};
}
QJsonObject completed(const QJsonObject& data) {
    return {{"state", "completed"}, {"data", data}};
}
bool onlyKeys(const QJsonObject& object, const QSet<QString>& allowed) {
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!allowed.contains(it.key())) return false;
    return true;
}

QJsonObject dispatch(const QStringList& arguments, QIODevice& input, WebHttpGet transport) {
    QString stateDir, operation;
    QSet<QString> seen;
    for (int index = 0; index < arguments.size(); index += 2) {
        const QString option = arguments[index];
        if ((option != "--state-dir" && option != "--operation") || seen.contains(option)
            || index + 1 == arguments.size() || arguments[index + 1].isEmpty())
            return failed("INVALID_ARGUMENT", "需要固定参数 --state-dir <绝对目录> --operation <status|search|read_page>");
        seen.insert(option);
        if (option == "--state-dir") stateDir = arguments[index + 1];
        else operation = arguments[index + 1];
    }
    if (!QDir::isAbsolutePath(stateDir) || stateDir.contains(QChar::Null)
        || !QStringList{"status", "search", "read_page"}.contains(operation))
        return failed("INVALID_ARGUMENT", "缓存目录必须为绝对路径，操作必须为 status、search 或 read_page");
    const QFileInfo directory(stateDir);
    if (directory.exists() && !directory.isDir())
        return failed("CACHE_ERROR", "配置的网页缓存路径不是目录");

    const QByteArray bytes = input.read(MaxInputBytes + 1);
    if (bytes.size() > MaxInputBytes)
        return failed("INVALID_ARGUMENT", "输入 JSON 不能超过 16 KiB");
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return failed("INVALID_ARGUMENT", "stdin 必须是一个有效的 JSON 对象");
    const auto parameters = document.object();

    if (operation == "status") {
        if (!parameters.isEmpty()) return failed("INVALID_ARGUMENT", "status 不接受参数");
        return completed({{"provider", "bing"}, {"cacheDirConfigured", true},
                          {"operations", QJsonArray{"status", "search", "read_page"}},
                          {"limitations", QJsonArray{
                              "仅检查本地路径配置，不代表网络可用；缓存首次读取时创建",
                              "匿名读取公开 HTTP/HTTPS；不使用登录或浏览器执行",
                              "Bing RSS 最多返回 5 条结果，不保证供应商可用性、相关性或高级查询支持",
                              "只提取服务器返回的 HTML/文本；不支持 PDF、登录页或需要 JavaScript 的正文",
                              "正文按页返回；快照保留 24 小时，最多 64 份"}}});
    }

    WebContentService service(stateDir, std::move(transport));
    WebContentResult result;
    if (operation == "search") {
        if (!onlyKeys(parameters, {"query", "limit"}) || !parameters["query"].isString())
            return failed("INVALID_ARGUMENT", "search 需要 query 字符串和可选的 limit 整数");
        int limit = 5;
        if (parameters.contains("limit")) {
            const auto value = parameters["limit"];
            const double number = value.toDouble();
            if (!value.isDouble() || number < 1 || number > 5 || std::floor(number) != number)
                return failed("INVALID_ARGUMENT", "limit 必须为 1–5 的整数");
            limit = int(number);
        }
        result = service.search(parameters["query"].toString(), limit);
    } else {
        if (!onlyKeys(parameters, {"url", "cursor"}) || !parameters["url"].isString())
            return failed("INVALID_ARGUMENT", "read_page 需要 url 字符串和可选的 cursor 字符串");
        if (parameters.contains("cursor") && (!parameters["cursor"].isString()
            || parameters["cursor"].toString().isEmpty() || parameters["cursor"].toString().size() > 256))
            return failed("INVALID_ARGUMENT", "cursor 必须为非空字符串，首次读取请省略");
        result = service.readPage(parameters["url"].toString(), parameters["cursor"].toString());
    }
    return result.success ? completed(result.data) : failed(result.errorCode, result.errorMessage);
}
}

int runWebCommand(const QStringList& arguments, QIODevice& input, QIODevice& output, WebHttpGet transport) {
    const auto result = dispatch(arguments, input, std::move(transport));
    const QByteArray bytes = QJsonDocument(result).toJson(QJsonDocument::Compact) + '\n';
    return output.write(bytes) == bytes.size() ? 0 : 1;
}
