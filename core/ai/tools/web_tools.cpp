#include "web_tools.h"
#include <cmath>

namespace {
ToolResult toToolResult(const WebContentResult& result) {
    if (result.success) return ToolResult::ok(result.data);
    auto failure = ToolResult::fail(result.errorCode + ": " + result.errorMessage);
    failure.data = {{"error", QJsonObject{{"code", result.errorCode}, {"message", result.errorMessage}}}};
    return failure;
}
ToolResult invalidArguments(const QString& message) {
    return toToolResult({false, {}, "INVALID_ARGUMENT", message});
}
bool boundedInteger(const QJsonObject& params, const QString& name, int minimum, int maximum) {
    if (!params.contains(name)) return true;
    const auto value = params[name];
    return value.isDouble() && value.toDouble() >= minimum && value.toDouble() <= maximum
        && std::floor(value.toDouble()) == value.toDouble();
}
}

WebFetchTool::WebFetchTool(std::shared_ptr<WebContentService> service)
    : AITool("web_fetch", "读取公开 HTTP/HTTPS 网页正文。长页返回 nextCursor；继续阅读时使用最初 URL 与游标。", ToolCategory::Query),
      m_service(service ? std::move(service) : std::make_shared<WebContentService>()) {}
QJsonObject WebFetchTool::parameterSchema() const {
    return {{"type", "object"}, {"required", QJsonArray{"url"}}, {"properties", QJsonObject{
        {"url", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 1024}, {"description", "首次读取的公开网页 URL，续读时保持相同"}}},
        {"cursor", QJsonObject{{"type", "string"}, {"maxLength", 80}, {"description", "上次返回的 nextCursor，省略则抓取新快照"}}},
        {"timeout_ms", QJsonObject{{"type", "integer"}, {"minimum", 1000}, {"maximum", 30000}, {"default", 15000}}}}}};
}
bool WebFetchTool::validate(const QJsonObject& params) const {
    return params["url"].isString() && !params["url"].toString().trimmed().isEmpty()
        && params["url"].toString().size() <= 1024
        && (!params.contains("cursor") || (params["cursor"].isString() && params["cursor"].toString().size() <= 80))
        && boundedInteger(params, "timeout_ms", 1000, 30000);
}
ToolResult WebFetchTool::execute(const QJsonObject& params) {
    if (!validate(params)) return invalidArguments("URL/游标类型或长度错误，超时须为 1000–30000 ms 整数");
    return toToolResult(m_service->readPage(params["url"].toString(), params["cursor"].toString(), params["timeout_ms"].toInt(15000)));
}

WebSearchTool::WebSearchTool(std::shared_ptr<WebContentService> service)
    : AITool("web_search", "使用 Bing 搜索公开网页，返回少量标题、完整 URL 和摘要。可调用 web_fetch 阅读来源正文。", ToolCategory::Query),
      m_service(service ? std::move(service) : std::make_shared<WebContentService>()) {}
QJsonObject WebSearchTool::parameterSchema() const {
    return {{"type", "object"}, {"required", QJsonArray{"query"}}, {"properties", QJsonObject{
        {"query", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 200}, {"description", "搜索关键词"}}},
        {"max_results", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 5}, {"default", 5}}}}}};
}
bool WebSearchTool::validate(const QJsonObject& params) const {
    return params["query"].isString() && !params["query"].toString().trimmed().isEmpty()
        && params["query"].toString().trimmed().size() <= 200 && boundedInteger(params, "max_results", 1, 5);
}
ToolResult WebSearchTool::execute(const QJsonObject& params) {
    if (!validate(params)) return invalidArguments("搜索关键词须为 1–200 字符，max_results 须为 1–5 整数");
    auto result = m_service->search(params["query"].toString(), params["max_results"].toInt(5));
    if (result.success) result.data.insert("results", result.data.take("entries"));
    return toToolResult(result);
}
