//
// Created by Claude on 2026/6/8.
// 网络工具 - 实现
//

#include "web_tools.h"
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QDebug>
#include <QHostInfo>

// ================================================================
// NetworkSecurityValidator 实现
// ================================================================

NetworkSecurityValidator::NetworkSecurityValidator() {}

bool NetworkSecurityValidator::isUrlAllowed(const QString& urlString) const {
    QUrl url(urlString);
    if (!url.isValid()) {
        return false;
    }

    // 仅允许 http 和 https
    const QString scheme = url.scheme().toLower();
    if (scheme != "http" && scheme != "https") {
        return false;
    }

    // 检查主机名
    const QString host = url.host();
    if (host.isEmpty()) {
        return false;
    }

    // 阻止 localhost 和 loopback
    if (host == "localhost" || host == "127.0.0.1" || host == "::1" || host == "0.0.0.0") {
        return false;
    }

    // 阻止 IP 地址访问内网
    if (isIpBlocked(host)) {
        return false;
    }

    return true;
}

bool NetworkSecurityValidator::isIpBlocked(const QString& host) const {
    // 如果是 IP 地址，直接检查
    QHostAddress addr;
    if (addr.setAddress(host)) {
        return isPrivateIp(addr) || isReservedIp(addr);
    }

    // 如果是主机名，解析后再检查
    QString resolvedIp = resolveHost(host);
    if (!resolvedIp.isEmpty()) {
        if (addr.setAddress(resolvedIp)) {
            return isPrivateIp(addr) || isReservedIp(addr);
        }
    }

    return false;
}

QString NetworkSecurityValidator::resolveHost(const QString& host) {
    QHostInfo info = QHostInfo::fromName(host);
    if (info.error() == QHostInfo::NoError && !info.addresses().isEmpty()) {
        return info.addresses().first().toString();
    }
    return QString();
}

bool NetworkSecurityValidator::isPrivateIp(const QHostAddress& addr) const {
    // 10.0.0.0/8
    quint32 ip = addr.toIPv4Address();
    if ((ip >> 24) == 10) {
        return true;
    }

    // 172.16.0.0/12 (172.16.0.0 - 172.31.255.255)
    // 高 8 位是 172，次高 8 位在 16-31 范围内
    if ((ip >> 24) == 172) {
        quint32 second = (ip >> 16) & 0xFF;
        if (second >= 16 && second <= 31) {
            return true;
        }
    }

    // 192.168.0.0/16
    if ((ip >> 16) == 0xC0A8) {  // 192.168.x.x
        return true;
    }

    return false;
}

bool NetworkSecurityValidator::isReservedIp(const QHostAddress& addr) const {
    // 127.0.0.0/8 (loopback)
    quint32 ip = addr.toIPv4Address();
    if ((ip >> 24) == 127) {
        return true;
    }

    // 169.254.0.0/16 (link-local)
    if ((ip >> 16) == 0xA9FE) {
        return true;
    }

    // 0.0.0.0/8
    if ((ip >> 24) == 0) {
        return true;
    }

    return false;
}

// ================================================================
// WebFetchTool 实现
// ================================================================

WebFetchTool::WebFetchTool()
    : AITool(
        "web_fetch",
        "获取指定 URL 的网页内容。支持 HTTP 和 HTTPS。"
        "返回纯文本内容，不包含 HTML 标签。",
        ToolCategory::Query
      ) {}

QJsonObject WebFetchTool::parameterSchema() const {
    QJsonObject schema;
    schema["type"] = "object";
    QJsonObject properties;
    QJsonObject url;
    url["type"] = "string";
    url["description"] = "要获取的网页 URL";
    properties["url"] = url;
    QJsonObject timeout_ms;
    timeout_ms["type"] = "integer";
    timeout_ms["description"] = "超时时间（毫秒），默认15000";
    timeout_ms["default"] = DEFAULT_TIMEOUT_MS;
    properties["timeout_ms"] = timeout_ms;
    schema["properties"] = properties;
    QJsonArray required;
    required.append("url");
    schema["required"] = required;
    return schema;
}

bool WebFetchTool::validate(const QJsonObject& params) const {
    if (!params.contains("url") || params.value("url").toString().isEmpty()) {
        return false;
    }
    return true;
}

ToolResult WebFetchTool::execute(const QJsonObject& params) {
    const QString urlString = params.value("url").toString();
    int timeoutMs = params.value("timeout_ms").toInt(DEFAULT_TIMEOUT_MS);

    // URL 安全检查
    if (!m_validator.isUrlAllowed(urlString)) {
        return ToolResult::fail(QString("URL 不允许访问: %1").arg(urlString));
    }

    QUrl url(urlString);
    if (!url.isValid()) {
        return ToolResult::fail(QString("无效的 URL: %1").arg(urlString));
    }

    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");

    QEventLoop loop;
    QNetworkReply* reply = manager.get(request);

    QTimer timer;
    timer.setSingleShot(true);
    timer.connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

    timer.start(timeoutMs);
    loop.exec();

    if (timer.isActive() == false) {
        reply->abort();
        reply->deleteLater();
        return ToolResult::fail(QString("请求超时 (%1 ms)").arg(timeoutMs));
    }

    if (reply->error() != QNetworkReply::NoError) {
        QString errorStr = reply->errorString();
        reply->deleteLater();
        return ToolResult::fail(QString("网络错误: %1").arg(errorStr));
    }

    const QByteArray data = reply->readAll();
    reply->deleteLater();

    // 检查响应大小
    if (data.size() > MAX_RESPONSE_SIZE) {
        return ToolResult::fail(QString("响应过大 (最大 %1 KB): %2 KB")
            .arg(MAX_RESPONSE_SIZE / 1024).arg(data.size() / 1024));
    }

    // 返回原始内容（让 LLM 决定如何解析）
    QJsonObject result;
    result["url"] = urlString;
    result["size"] = data.size();
    result["content"] = QString::fromUtf8(data);

    return ToolResult::ok(result);
}

// ================================================================
// WebSearchTool 实现
// ================================================================

WebSearchTool::WebSearchTool()
    : AITool(
        "web_search",
        "使用 Bing 搜索获取相关信息。返回搜索结果列表，包括标题和摘要。",
        ToolCategory::Query
      ) {}

QJsonObject WebSearchTool::parameterSchema() const {
    QJsonObject schema;
    schema["type"] = "object";
    QJsonObject properties;
    QJsonObject query;
    query["type"] = "string";
    query["description"] = "搜索关键词";
    properties["query"] = query;
    QJsonObject max_results;
    max_results["type"] = "integer";
    max_results["description"] = "最大结果数，默认5";
    max_results["default"] = DEFAULT_MAX_RESULTS;
    properties["max_results"] = max_results;
    schema["properties"] = properties;
    QJsonArray required;
    required.append("query");
    schema["required"] = required;
    return schema;
}

bool WebSearchTool::validate(const QJsonObject& params) const {
    if (!params.contains("query") || params.value("query").toString().isEmpty()) {
        return false;
    }
    return true;
}

ToolResult WebSearchTool::execute(const QJsonObject& params) {
    const QString query = params.value("query").toString();
    int maxResults = params.value("max_results").toInt(DEFAULT_MAX_RESULTS);

    if (query.trimmed().isEmpty()) {
        return ToolResult::fail("搜索关键词不能为空");
    }

    const QString results = bingSearch(query, maxResults);

    QJsonObject result;
    result["query"] = query;
    result["results"] = results;

    return ToolResult::ok(result);
}

QString WebSearchTool::bingSearch(const QString& query, int maxResults) const {
    // 构建 Bing 搜索 URL
    QUrl url("https://cn.bing.com/search");
    QUrlQuery queryParams;
    queryParams.addQueryItem("q", query);
    queryParams.addQueryItem("ensearch", "1");  // 启用国内版
    url.setQuery(queryParams);

    if (!m_validator.isUrlAllowed(url.toString())) {
        return QString("搜索 URL 不安全");
    }

    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");

    QEventLoop loop;
    QNetworkReply* reply = manager.get(request);

    QTimer timer;
    timer.setSingleShot(true);
    timer.connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

    timer.start(BING_TIMEOUT_MS);
    loop.exec();

    if (timer.isActive() == false) {
        reply->abort();
        reply->deleteLater();
        return QString("搜索超时，请稍后重试");
    }

    if (reply->error() != QNetworkReply::NoError) {
        QString errorStr = reply->errorString();
        reply->deleteLater();
        return QString("搜索失败: %1").arg(errorStr);
    }

    const QByteArray html = reply->readAll();
    reply->deleteLater();

    // 解析 Bing 结果
    QJsonArray results = parseBingResults(QString::fromUtf8(html));

    // 格式化为文本输出
    QStringList output;
    output.append(QString("搜索结果: %1\n").arg(query));

    int count = qMin(results.size(), maxResults);
    for (int i = 0; i < count; ++i) {
        const QJsonObject& item = results[i].toObject();
        QString title = item.value("title").toString();
        QString snippet = item.value("snippet").toString();
        QString resultUrl = item.value("url").toString();

        if (!title.isEmpty()) {
            output.append(QString("[%1] %2").arg(i + 1).arg(title));
        }
        if (!snippet.isEmpty()) {
            output.append(QString("   摘要: %1").arg(snippet));
        }
        if (!resultUrl.isEmpty()) {
            output.append(QString("   链接: %1").arg(resultUrl));
        }
        output.append("");
    }

    if (output.isEmpty()) {
        return QString("未找到相关结果");
    }

    return output.join("\n");
}

QJsonArray WebSearchTool::parseBingResults(const QString& html) const {
    QJsonArray results;

    // 使用更简单的正则模式，避免 raw string 中的引号问题
    // 匹配 <li class="sa-item"> 或 <div class="b_algo">
    QRegularExpression liRegex("<li[^>]*class=\"([^\"]*sa-item[^\"]*)\"[^>]*>([\\s\\S]*?)</li>");
    QRegularExpression divRegex("<div[^>]*class=\"([^\"]*b_algo[^\"]*)\"[^>]*>([\\s\\S]*?)</div>");

    QStringList blocks;

    QRegularExpressionMatchIterator it = liRegex.globalMatch(html);
    while (it.hasNext()) {
        blocks.append(it.next().captured(2));
    }

    it = divRegex.globalMatch(html);
    while (it.hasNext()) {
        blocks.append(it.next().captured(2));
    }

    // 解析每个结果块 - 提取标题和链接
    QRegularExpression titleRegex("<h2[^>]*>.*?<a[^>]*>([^<]*)</a>");
    QRegularExpression linkRegex("<a[^>]*href=\"([^\"]*)\"[^>]*>");

    int count = 0;
    for (const QString& block : blocks) {
        if (count >= 10) break;

        QRegularExpressionMatch titleMatch = titleRegex.match(block);
        QRegularExpressionMatch linkMatch = linkRegex.match(block);

        if (titleMatch.hasMatch()) {
            QJsonObject item;
            item["title"] = cleanHtml(titleMatch.captured(1));

            if (linkMatch.hasMatch()) {
                QString url = linkMatch.captured(1);
                // 过滤掉 Bing 内部链接
                if (!url.startsWith("/") && !url.contains("bing.com")) {
                    item["url"] = url;
                }
            }

            if (!item.value("title").toString().isEmpty()) {
                results.append(item);
                count++;
            }
        }
    }

    return results;
}

QString WebSearchTool::cleanHtml(const QString& html) const {
    QString text = html;
    // 移除 HTML 标签
    text.remove(QRegularExpression("<[^>]+>"));
    // 替换 HTML 实体
    text.replace("&nbsp;", " ");
    text.replace("&amp;", "&");
    text.replace("&lt;", "<");
    text.replace("&gt;", ">");
    text.replace("&quot;", "\"");
    text.replace("&#39;", "'");
    // 清理多余空白
    text = text.simplified();
    return text;
}