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
#include <algorithm>
#include <iterator>

namespace {
struct SafeHttpResult {
    bool success = false;
    QByteArray data;
    QString error;
    QUrl finalUrl;
};

QByteArray hostHeaderForUrl(const QUrl& url) {
    QString host = url.host();
    if (host.contains(':') && !host.startsWith('[')) {
        host = '[' + host + ']';
    }
    const int port = url.port();
    const bool defaultPort = port < 0
        || (url.scheme() == "http" && port == 80)
        || (url.scheme() == "https" && port == 443);
    if (!defaultPort) {
        host += ':' + QString::number(port);
    }
    return host.toUtf8();
}

SafeHttpResult performSafeGet(const QUrl& initialUrl,
                              const NetworkSecurityValidator& validator,
                              int timeoutMs,
                              qint64 maxResponseSize) {
    QUrl logicalUrl = initialUrl;
    constexpr int maxRedirects = 5;

    for (int redirectCount = 0; redirectCount <= maxRedirects; ++redirectCount) {
        QHostAddress address;
        QString validationError;
        if (!validator.resolveAllowedAddress(logicalUrl, &address, &validationError)) {
            return {false, {}, validationError, logicalUrl};
        }

        QUrl requestUrl = logicalUrl;
        requestUrl.setHost(address.toString());
        QNetworkRequest request(requestUrl);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::ManualRedirectPolicy);
        request.setHeader(QNetworkRequest::UserAgentHeader,
                          "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
        request.setRawHeader("Host", hostHeaderForUrl(logicalUrl));
        request.setTransferTimeout(timeoutMs);

        if (logicalUrl.scheme().compare("https", Qt::CaseInsensitive) == 0) {
            request.setPeerVerifyName(logicalUrl.host());
        }

        QNetworkAccessManager manager;
        QEventLoop loop;
        QNetworkReply* reply = manager.get(request);
        QTimer timer;
        timer.setSingleShot(true);
        bool oversized = false;
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(reply, &QNetworkReply::downloadProgress, reply,
                         [reply, maxResponseSize, &oversized](qint64 received, qint64 total) {
            if (received > maxResponseSize || (total > 0 && total > maxResponseSize)) {
                oversized = true;
                reply->abort();
            }
        });
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        timer.start(timeoutMs);
        loop.exec();

        if (!timer.isActive()) {
            reply->abort();
            reply->deleteLater();
            return {false, {}, QString("请求超时 (%1 ms)").arg(timeoutMs), logicalUrl};
        }
        timer.stop();

        if (oversized) {
            reply->deleteLater();
            return {false, {},
                    QString("Response exceeds size limit (%1 KB)").arg(maxResponseSize / 1024),
                    logicalUrl};
        }

        const QVariant redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute);
        if (redirect.isValid()) {
            const QUrl target = logicalUrl.resolved(redirect.toUrl());
            reply->deleteLater();
            if (redirectCount == maxRedirects) {
                return {false, {}, "重定向次数过多", logicalUrl};
            }
            logicalUrl = target;
            continue;
        }

        if (reply->error() != QNetworkReply::NoError) {
            const QString error = QString("网络错误: %1").arg(reply->errorString());
            reply->deleteLater();
            return {false, {}, error, logicalUrl};
        }

        const QByteArray data = reply->readAll();
        reply->deleteLater();
        if (data.size() > maxResponseSize) {
            return {false, {},
                    QString("响应过大 (最大 %1 KB): %2 KB")
                        .arg(maxResponseSize / 1024)
                        .arg(data.size() / 1024),
                    logicalUrl};
        }
        return {true, data, {}, logicalUrl};
    }

    return {false, {}, "请求未完成", logicalUrl};
}
}

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

    return resolveAllowedAddress(url, nullptr);
}

bool NetworkSecurityValidator::isIpBlocked(const QString& host) const {
    QHostAddress addr;
    if (addr.setAddress(host)) {
        return isAddressBlocked(addr);
    }

    const QList<QHostAddress> addresses = resolveHostAddresses(host);
    if (addresses.isEmpty()) {
        return true;
    }
    for (const QHostAddress& resolved : addresses) {
        if (isAddressBlocked(resolved)) {
            return true;
        }
    }
    return false;
}

QList<QHostAddress> NetworkSecurityValidator::resolveHostAddresses(const QString& host) {
    QHostInfo info = QHostInfo::fromName(host);
    if (info.error() == QHostInfo::NoError) {
        return info.addresses();
    }
    return {};
}

bool NetworkSecurityValidator::resolveAllowedAddress(const QUrl& url,
                                                     QHostAddress* address,
                                                     QString* errorMessage) const {
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || (scheme != "http" && scheme != "https") || url.host().isEmpty()) {
        if (errorMessage) *errorMessage = QString("URL 不允许访问: %1").arg(url.toString());
        return false;
    }

    QList<QHostAddress> addresses;
    QHostAddress literal;
    if (literal.setAddress(url.host())) {
        addresses.append(literal);
    } else {
        addresses = resolveHostAddresses(url.host());
    }
    if (addresses.isEmpty()) {
        if (errorMessage) *errorMessage = QString("无法安全解析主机: %1").arg(url.host());
        return false;
    }
    for (const QHostAddress& candidate : addresses) {
        if (isAddressBlocked(candidate)) {
            if (errorMessage) *errorMessage = QString("主机解析到受限地址: %1").arg(candidate.toString());
            return false;
        }
    }
    if (address) *address = addresses.first();
    return true;
}

bool NetworkSecurityValidator::isAddressBlocked(const QHostAddress& addr) const {
    bool isIpv4 = false;
    const quint32 ipv4 = addr.toIPv4Address(&isIpv4);
    if (isIpv4) {
        return isBlockedIpv4(ipv4);
    }
    return addr.protocol() != QAbstractSocket::IPv6Protocol
        || isBlockedIpv6(addr.toIPv6Address());
}

bool NetworkSecurityValidator::isBlockedIpv4(quint32 ip) const {
    const quint8 first = static_cast<quint8>(ip >> 24);
    const quint8 second = static_cast<quint8>((ip >> 16) & 0xff);
    return first == 0
        || first == 10
        || first == 127
        || (first == 100 && second >= 64 && second <= 127)
        || (first == 169 && second == 254)
        || (first == 172 && second >= 16 && second <= 31)
        || (first == 192 && second == 168)
        || first >= 224;
}

bool NetworkSecurityValidator::isBlockedIpv6(const Q_IPV6ADDR& ip) const {
    const bool unspecified = std::all_of(std::begin(ip.c), std::end(ip.c), [](quint8 byte) { return byte == 0; });
    const bool loopback = std::all_of(std::begin(ip.c), std::end(ip.c) - 1, [](quint8 byte) { return byte == 0; })
        && ip.c[15] == 1;
    const bool uniqueLocal = (ip.c[0] & 0xfe) == 0xfc;
    const bool linkLocal = ip.c[0] == 0xfe && (ip.c[1] & 0xc0) == 0x80;
    const bool multicast = ip.c[0] == 0xff;
    return unspecified || loopback || uniqueLocal || linkLocal || multicast;
}

// ================================================================
// WebFetchTool 实现
// ================================================================

WebFetchTool::WebFetchTool()
    : AITool(
        "web_fetch",
        "获取指定 URL 的网页内容。支持 HTTP 和 HTTPS。"
        "返回响应中的原始文本内容。",
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

    QUrl url(urlString);
    if (!url.isValid()) {
        return ToolResult::fail(QString("无效的 URL: %1").arg(urlString));
    }

    const SafeHttpResult response = performSafeGet(url, m_validator, timeoutMs, MAX_RESPONSE_SIZE);
    if (!response.success) return ToolResult::fail(response.error);

    // 返回原始内容（让 LLM 决定如何解析）
    QJsonObject result;
    result["url"] = response.finalUrl.toString();
    result["size"] = response.data.size();
    result["content"] = QString::fromUtf8(response.data);

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

    const SafeHttpResult response = performSafeGet(url, m_validator, BING_TIMEOUT_MS, 2 * 1024 * 1024);
    if (!response.success) return QString("搜索失败: %1").arg(response.error);

    // 解析 Bing 结果
    QJsonArray results = parseBingResults(QString::fromUtf8(response.data));

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
