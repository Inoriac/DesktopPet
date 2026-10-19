#include "web_content_service.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QStringConverter>
#include <QTextDocument>
#include <QTimer>
#include <QUuid>
#include <QXmlStreamReader>
#include <algorithm>
#include <iterator>

namespace {
constexpr qint64 MaxResponseSize = 500 * 1024;
constexpr int MaxDataLength = 3800;
constexpr int MaxTextLength = 1800;
constexpr int MaxUrlLength = 1024;
constexpr int CacheLifetimeSeconds = 24 * 60 * 60;
constexpr int MaxSnapshots = 64;

QString boundedText(const QString& value, qsizetype limit) {
    qsizetype end = qMin(value.size(), limit);
    if (end > 0 && end < value.size() && value[end - 1].isHighSurrogate()) --end;
    return value.left(end);
}

WebContentResult fail(const QString& code, const QString& message) {
    return {false, {}, code, boundedText(message, MaxTextLength)};
}
WebHttpResponse httpFail(const QUrl& url, const QString& code, const QString& message, int status = 0) {
    return {false, {}, {}, url, code, boundedText(message, MaxTextLength), status};
}

bool publicUrlSyntax(const QUrl& url) {
    if (!url.isValid() || url.isRelative() || url.host().isEmpty()
        || (url.scheme() != "http" && url.scheme() != "https")
        || !url.userInfo().isEmpty() || url.port() == 0
        || url.toString().size() > MaxUrlLength) return false;
    const QString host = url.host().toLower();
    if (host == "localhost" || host.endsWith(".localhost") || host.endsWith(".local")) return false;
    QHostAddress literal;
    return !literal.setAddress(host) || !NetworkSecurityValidator().isIpBlocked(host);
}

QByteArray hostHeaderForUrl(const QUrl& url) {
    QString host = url.host();
    if (host.contains(':')) host = '[' + host + ']';
    const int port = url.port();
    if (port >= 0 && !(url.scheme() == "http" && port == 80) && !(url.scheme() == "https" && port == 443))
        host += ':' + QString::number(port);
    return host.toUtf8();
}

WebHttpResponse performSafeGet(const QUrl& initialUrl, int timeoutMs, qint64 maxResponseSize) {
    QElapsedTimer elapsed;
    elapsed.start();
    auto remaining = [&] { return qMax(0, timeoutMs - int(elapsed.elapsed())); };
    QUrl logicalUrl = initialUrl;
    const NetworkSecurityValidator validator;
    constexpr int maxRedirects = 5;
    for (int redirects = 0; redirects <= maxRedirects; ++redirects) {
        if (!publicUrlSyntax(logicalUrl)) return httpFail(logicalUrl, "URL_NOT_ALLOWED", "URL 不允许访问");
        if (remaining() == 0) return httpFail(logicalUrl, "TIMEOUT", "网络请求超时");
        QList<QHostAddress> addresses;
        QHostAddress literal;
        if (literal.setAddress(logicalUrl.host())) {
            addresses.append(literal);
        } else {
            QEventLoop lookupLoop;
            QTimer lookupTimer;
            lookupTimer.setSingleShot(true);
            bool completed = false;
            const int lookupId = QHostInfo::lookupHost(logicalUrl.host(), &lookupLoop, [&](const QHostInfo& info) {
                completed = true;
                if (info.error() == QHostInfo::NoError) addresses = info.addresses();
                lookupLoop.quit();
            });
            QObject::connect(&lookupTimer, &QTimer::timeout, &lookupLoop, &QEventLoop::quit);
            lookupTimer.start(remaining());
            lookupLoop.exec();
            if (!completed) {
                QHostInfo::abortHostLookup(lookupId);
                return httpFail(logicalUrl, "TIMEOUT", "主机解析超时");
            }
        }
        if (addresses.isEmpty()) return httpFail(logicalUrl, "NETWORK_ERROR", "无法解析主机");
        for (const auto& address : addresses)
            if (validator.isIpBlocked(address.toString()))
                return httpFail(logicalUrl, "URL_NOT_ALLOWED", "主机解析到受限地址");
        if (remaining() == 0) return httpFail(logicalUrl, "TIMEOUT", "网络请求超时");

        QUrl requestUrl = logicalUrl;
        requestUrl.setHost(addresses.first().toString());
        QNetworkRequest request(requestUrl);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        // HTTP/2 uses the pinned IP as :authority; HTTP/1.1 preserves the logical Host.
        request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
        request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
        request.setHeader(QNetworkRequest::UserAgentHeader, "Mozilla/5.0 DesktopPet/1.0");
        request.setRawHeader("Host", hostHeaderForUrl(logicalUrl));
        request.setTransferTimeout(remaining());
        if (logicalUrl.scheme() == "https") request.setPeerVerifyName(logicalUrl.host());
        QNetworkAccessManager manager;
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        QNetworkReply* reply = manager.get(request);
        QByteArray body;
        bool oversized = false;
        auto drain = [&] {
            body += reply->readAll();
            if (body.size() > maxResponseSize) { oversized = true; reply->abort(); }
        };
        QObject::connect(reply, &QNetworkReply::readyRead, &loop, drain);
        QObject::connect(reply, &QNetworkReply::downloadProgress, &loop, [&](qint64 received, qint64 total) {
            if (received > maxResponseSize || total > maxResponseSize) { oversized = true; reply->abort(); }
        });
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(remaining());
        if (!reply->isFinished()) loop.exec();
        if (!timer.isActive() || remaining() == 0) {
            reply->abort();
            return httpFail(logicalUrl, "TIMEOUT", "网络请求超时");
        }
        timer.stop();
        drain();
        if (oversized) return httpFail(logicalUrl, "RESPONSE_TOO_LARGE", "响应超过 500 KiB 限制");
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QVariant redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute);
        if (redirect.isValid() && status >= 300 && status < 400) {
            if (redirects == maxRedirects) return httpFail(logicalUrl, "NETWORK_ERROR", "重定向次数过多", status);
            logicalUrl = logicalUrl.resolved(redirect.toUrl());
            continue;
        }
        if (status >= 300) return httpFail(logicalUrl, "HTTP_ERROR", QString("HTTP %1").arg(status), status);
        if (reply->error() != QNetworkReply::NoError)
            return httpFail(logicalUrl, "NETWORK_ERROR", reply->errorString(), status);
        return {true, body, reply->rawHeader("Content-Type"), logicalUrl, {}, {}, status};
    }
    return httpFail(logicalUrl, "NETWORK_ERROR", "请求未完成");
}

WebContentResult checkResponse(const WebHttpResponse& response) {
    if (!response.success) return fail(response.errorCode.isEmpty() ? "NETWORK_ERROR" : response.errorCode,
                                      response.errorMessage.isEmpty() ? "网络请求失败" : response.errorMessage);
    if (response.statusCode < 200 || response.statusCode >= 300)
        return fail("HTTP_ERROR", QString("HTTP %1").arg(response.statusCode));
    if (response.body.size() > MaxResponseSize) return fail("RESPONSE_TOO_LARGE", "响应超过 500 KiB 限制");
    if (!publicUrlSyntax(response.finalUrl)) return fail("URL_NOT_ALLOWED", "最终 URL 不允许访问或过长");
    return {true, {}, {}, {}};
}

// HTML is only text input. Never resolve images, stylesheets or file resources.
class TextOnlyDocument final : public QTextDocument {
protected:
    QVariant loadResource(int, const QUrl&) override { return {}; }
};
QString htmlText(const QString& html) {
    TextOnlyDocument document;
    QFont font;
    font.setPixelSize(16);
    document.setDefaultFont(font);
    document.setHtml(html);
    QString text = document.toPlainText();
    text.remove(QChar::ObjectReplacementCharacter);
    text.replace(QChar::Nbsp, ' ');
    return text.trimmed();
}

struct ElementRange { qsizetype start = -1, bodyStart = 0, bodyEnd = 0, end = 0; };
ElementRange elementRange(const QString& html, const QString& name, qsizetype from = 0) {
    const QRegularExpression tags("<\\s*(/?)\\s*" + name + "\\b(?:\"[^\"]*\"|'[^']*'|[^'\">])*>",
                                  QRegularExpression::CaseInsensitiveOption);
    auto matches = tags.globalMatch(html, from);
    ElementRange range;
    int depth = 0;
    while (matches.hasNext()) {
        const auto match = matches.next();
        const bool closing = !match.captured(1).isEmpty();
        const bool selfClosing = match.captured().endsWith("/>");
        if (range.start < 0) {
            if (closing) continue;
            range.start = match.capturedStart();
            range.bodyStart = match.capturedEnd();
            if (selfClosing) return {range.start, range.bodyStart, range.bodyStart, range.bodyStart};
            depth = 1;
        } else if (closing && --depth == 0) {
            range.bodyEnd = match.capturedStart();
            range.end = match.capturedEnd();
            return range;
        } else if (!closing && !selfClosing) {
            ++depth;
        }
    }
    if (range.start >= 0) { range.bodyEnd = html.size(); range.end = html.size(); }
    return range;
}
QString elementBody(const QString& html, const ElementRange& range) {
    return html.mid(range.bodyStart, range.bodyEnd - range.bodyStart);
}
QString readableHtml(QString html, QString* title) {
    html.remove(QRegularExpression("<!--[\\s\\S]*?-->"));
    const auto titleRange = elementRange(html, "title");
    if (titleRange.start >= 0) *title = boundedText(htmlText(elementBody(html, titleRange)).simplified(), 160);
    for (const QString& tag : {"script", "style", "template", "nav", "header", "footer", "aside", "form", "head", "svg"}) {
        qsizetype from = 0;
        while (true) {
            const auto range = elementRange(html, tag, from);
            if (range.start < 0) break;
            html.remove(range.start, range.end - range.start);
            from = range.start;
        }
    }
    for (const QString& tag : {"main", "article", "body"}) {
        const auto range = elementRange(html, tag);
        if (range.start >= 0) { html = elementBody(html, range); break; }
    }
    return htmlText(html);
}

WebContentResult decodePage(const WebHttpResponse& response, QString* text, QString* title, QString* contentType) {
    const QString header = QString::fromLatin1(response.contentType);
    *contentType = header.section(';', 0, 0).trimmed().toLower();
    const bool html = *contentType == "text/html" || *contentType == "application/xhtml+xml";
    if (!html && *contentType != "text/plain") return fail("UNSUPPORTED_CONTENT", "仅支持 HTML 或 text/plain 正文");
    const QRegularExpression charsetExpression("charset\\s*=\\s*[\"']?([^\\s;\"'>]+)", QRegularExpression::CaseInsensitiveOption);
    auto charsetMatch = charsetExpression.match(header);
    QString charset = charsetMatch.hasMatch() ? charsetMatch.captured(1) : QString();
    if (charset.isEmpty() && html) {
        const QString prefix = QString::fromLatin1(response.body.left(4096));
        const QRegularExpression metaExpression("<meta\\b[^>]*>", QRegularExpression::CaseInsensitiveOption);
        auto metas = metaExpression.globalMatch(prefix);
        while (metas.hasNext()) {
            charsetMatch = charsetExpression.match(metas.next().captured());
            if (charsetMatch.hasMatch()) { charset = charsetMatch.captured(1); break; }
        }
    }
    if (charset.isEmpty()) {
        const auto bomEncoding = QStringConverter::encodingForData(response.body);
        charset = bomEncoding ? QString::fromLatin1(QStringConverter::nameForEncoding(*bomEncoding)) : "UTF-8";
    }
    QStringDecoder decoder(charset.toLatin1(), QStringConverter::Flag::Stateless);
    if (!decoder.isValid()) return fail("UNSUPPORTED_ENCODING", "不支持网页声明的字符编码: " + boundedText(charset, 100));
    const QString decoded = decoder(response.body);
    if (decoder.hasError()) return fail("INVALID_ENCODING", "网页正文不符合声明的字符编码");
    *text = html ? readableHtml(decoded, title) : decoded;
    if (text->trimmed().isEmpty()) return fail("NO_READABLE_CONTENT", "页面没有可读正文，可能需要 JavaScript 或登录");
    return {true, {}, {}, {}};
}

int serializedLength(const QJsonObject& data) {
    return QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact)).size();
}
QJsonObject pageData(const QJsonObject& snapshot, const QString& id, qsizetype offset, qsizetype length) {
    const QString text = snapshot["content"].toString();
    const qsizetype next = offset + length;
    const bool more = next < text.size();
    return {{"title", snapshot["title"]}, {"url", snapshot["url"]}, {"size", snapshot["size"]},
            {"contentType", snapshot["contentType"]}, {"content", text.mid(offset, length)},
            {"hasMore", more}, {"nextCursor", more ? id + ':' + QString::number(next) : QString()}};
}
WebContentResult paginate(const QJsonObject& snapshot, const QString& id, qsizetype offset) {
    const QString text = snapshot["content"].toString();
    qsizetype low = 0, high = qMin(qsizetype(MaxTextLength), text.size() - offset);
    while (low < high) {
        const qsizetype middle = (low + high + 1) / 2;
        if (serializedLength(pageData(snapshot, id, offset, middle)) <= MaxDataLength) low = middle;
        else high = middle - 1;
    }
    if (offset + low < text.size() && low > 0 && text[offset + low - 1].isHighSurrogate()) --low;
    if (low <= 0) return fail("CACHE_ERROR", "快照元信息超过输出限制");
    return {true, pageData(snapshot, id, offset, low), {}, {}};
}

bool expired(const QJsonObject& snapshot) {
    const auto created = QDateTime::fromString(snapshot["createdAt"].toString(), Qt::ISODateWithMs);
    return !created.isValid() || created.secsTo(QDateTime::currentDateTimeUtc()) >= CacheLifetimeSeconds;
}
QJsonObject readSnapshot(const QString& path) {
    QFile file(path);
    if (QFileInfo(path).isSymLink() || !file.open(QIODevice::ReadOnly) || file.size() > 8 * 1024 * 1024) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
void pruneSnapshots(const QString& cacheDir) {
    QDir directory(cacheDir);
    const QRegularExpression name("^[a-f0-9]{32}\\.json$");
    const auto files = directory.entryInfoList({"*.json"}, QDir::Files, QDir::Time | QDir::Reversed);
    QStringList retained;
    for (const auto& file : files) {
        if (!name.match(file.fileName()).hasMatch()) continue;
        if (expired(readSnapshot(file.filePath()))) QFile::remove(file.filePath());
        else retained.append(file.filePath());
    }
    while (retained.size() >= MaxSnapshots) QFile::remove(retained.takeFirst());
}
}

bool NetworkSecurityValidator::isUrlAllowed(const QString& url) const {
    return resolveAllowedAddress(QUrl(url, QUrl::StrictMode), nullptr);
}
bool NetworkSecurityValidator::isIpBlocked(const QString& host) const {
    QHostAddress literal;
    if (literal.setAddress(host)) return isAddressBlocked(literal);
    const auto addresses = resolveHostAddresses(host);
    return addresses.isEmpty() || std::any_of(addresses.begin(), addresses.end(), [this](const auto& address) { return isAddressBlocked(address); });
}
QList<QHostAddress> NetworkSecurityValidator::resolveHostAddresses(const QString& host) {
    const auto info = QHostInfo::fromName(host);
    return info.error() == QHostInfo::NoError ? info.addresses() : QList<QHostAddress>{};
}
bool NetworkSecurityValidator::resolveAllowedAddress(const QUrl& url, QHostAddress* address, QString* errorMessage) const {
    if (!publicUrlSyntax(url)) {
        if (errorMessage) *errorMessage = "URL 不允许访问";
        return false;
    }
    QHostAddress literal;
    const auto addresses = literal.setAddress(url.host()) ? QList<QHostAddress>{literal} : resolveHostAddresses(url.host());
    if (addresses.isEmpty()) {
        if (errorMessage) *errorMessage = "无法安全解析主机";
        return false;
    }
    for (const auto& candidate : addresses) {
        if (isAddressBlocked(candidate)) {
            if (errorMessage) *errorMessage = "主机解析到受限地址";
            return false;
        }
    }
    if (address) *address = addresses.first();
    return true;
}
bool NetworkSecurityValidator::isAddressBlocked(const QHostAddress& address) const {
    bool ipv4 = false;
    const auto value = address.toIPv4Address(&ipv4);
    return ipv4 ? isBlockedIpv4(value) : address.protocol() != QAbstractSocket::IPv6Protocol || isBlockedIpv6(address.toIPv6Address());
}
bool NetworkSecurityValidator::isBlockedIpv4(quint32 ip) const {
    const quint8 first = quint8(ip >> 24), second = quint8((ip >> 16) & 0xff);
    return first == 0 || first == 10 || first == 127 || (first == 100 && second >= 64 && second <= 127)
        || (first == 169 && second == 254) || (first == 172 && second >= 16 && second <= 31)
        || (first == 192 && second == 168) || first >= 224;
}
bool NetworkSecurityValidator::isBlockedIpv6(const Q_IPV6ADDR& ip) const {
    const bool zeroPrefix = std::all_of(std::begin(ip.c), std::end(ip.c) - 1, [](quint8 b) { return b == 0; });
    return (zeroPrefix && ip.c[15] <= 1) || (ip.c[0] & 0xfe) == 0xfc
        || (ip.c[0] == 0xfe && (ip.c[1] & 0xc0) == 0x80) || ip.c[0] == 0xff;
}

WebContentService::WebContentService(QString cacheDir, WebHttpGet transport)
    : m_cacheDir(cacheDir.isEmpty() ? QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).filePath("web") : std::move(cacheDir)),
      m_transport(transport ? std::move(transport) : performSafeGet) {}

WebContentResult WebContentService::search(const QString& query, int limit) {
    const QString normalized = query.trimmed();
    if (normalized.isEmpty() || normalized.size() > 200 || limit < 1 || limit > 5)
        return fail("INVALID_ARGUMENT", "搜索关键词须为 1–200 字符，结果数须为 1–5");
    QUrl url("https://cn.bing.com/search");
    url.setQuery("q=" + QString::fromLatin1(QUrl::toPercentEncoding(normalized)) + "&ensearch=1&format=rss", QUrl::StrictMode);
    const auto response = m_transport(url, 15000, MaxResponseSize);
    const auto checked = checkResponse(response);
    if (!checked.success) return checked;
    QXmlStreamReader xml(response.body);
    QJsonArray entries;
    QSet<QString> seen;
    bool channelSeen = false, limited = false, budgetReached = false;
    if (!xml.readNextStartElement() || xml.name() != u"rss") return fail("INVALID_FEED", "搜索响应不是 RSS");
    while (xml.readNextStartElement()) {
        if (xml.name() != u"channel") { xml.skipCurrentElement(); continue; }
        if (channelSeen) return fail("INVALID_FEED", "搜索响应有多个 channel");
        channelSeen = true;
        while (xml.readNextStartElement()) {
            if (xml.name() != u"item") { xml.skipCurrentElement(); continue; }
            QString title, link, snippet;
            while (xml.readNextStartElement()) {
                const auto name = xml.name();
                if (name == u"title") title = xml.readElementText(QXmlStreamReader::IncludeChildElements);
                else if (name == u"link") link = xml.readElementText().trimmed();
                else if (name == u"description") snippet = xml.readElementText(QXmlStreamReader::IncludeChildElements);
                else xml.skipCurrentElement();
            }
            const QUrl sourceUrl(link, QUrl::StrictMode);
            if (!publicUrlSyntax(sourceUrl)) { limited = true; continue; }
            const QString address = sourceUrl.toString();
            if (seen.contains(address)) continue;
            seen.insert(address);
            if (entries.size() == limit || budgetReached) { limited = true; continue; }
            const QJsonObject entry{{"title", boundedText(htmlText(title).simplified(), 160)},
                                    {"url", address}, {"snippet", boundedText(htmlText(snippet).simplified(), 240)}};
            QJsonArray candidate = entries;
            candidate.append(entry);
            const QJsonObject data{{"query", normalized}, {"provider", "bing"}, {"entries", candidate}, {"limited", false}};
            if (serializedLength(data) > MaxDataLength) { limited = true; budgetReached = true; continue; }
            entries = candidate;
        }
    }
    // Consume trailing tokens so a valid prefix followed by malformed XML is not accepted.
    while (!xml.atEnd()) xml.readNext();
    if (xml.hasError() || !channelSeen) return fail("INVALID_FEED", "搜索 RSS 无效或不完整");
    return {true, {{"query", normalized}, {"provider", "bing"}, {"entries", entries}, {"limited", limited}}, {}, {}};
}

WebContentResult WebContentService::readPage(const QString& url, const QString& cursor, int timeoutMs) {
    const QString inputUrl = url.trimmed();
    const QUrl parsed(inputUrl, QUrl::StrictMode);
    if (inputUrl.isEmpty() || timeoutMs < 1000 || timeoutMs > 30000) return fail("INVALID_ARGUMENT", "URL 不能为空，超时须为 1000–30000 ms");
    if (inputUrl.size() > MaxUrlLength || !publicUrlSyntax(parsed)) return fail("URL_NOT_ALLOWED", "URL 不允许访问或超过 1024 字符");
    if (!cursor.isEmpty()) {
        const QRegularExpression format("^([a-f0-9]{32}):([1-9][0-9]{0,6})$");
        const auto match = format.match(cursor);
        if (!match.hasMatch()) return fail("INVALID_CURSOR", "网页游标无效");
        const auto snapshot = readSnapshot(QDir(m_cacheDir).filePath(match.captured(1) + ".json"));
        const qsizetype offset = match.captured(2).toInt();
        const QString content = snapshot["content"].toString();
        if (snapshot["inputUrl"].toString() != inputUrl || expired(snapshot) || offset >= content.size()
            || content[offset].isLowSurrogate()) return fail("INVALID_CURSOR", "网页快照已过期、缺失或与 URL 不匹配");
        return paginate(snapshot, match.captured(1), offset);
    }
    const auto response = m_transport(parsed, timeoutMs, MaxResponseSize);
    const auto checked = checkResponse(response);
    if (!checked.success) return checked;
    QString content, title, contentType;
    const auto decoded = decodePage(response, &content, &title, &contentType);
    if (!decoded.success) return decoded;
    const QJsonObject snapshot{{"inputUrl", inputUrl}, {"url", response.finalUrl.toString()}, {"title", title},
                              {"size", response.body.size()}, {"contentType", contentType}, {"content", content},
                              {"createdAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
    if (!QDir().mkpath(m_cacheDir)) return fail("CACHE_ERROR", "无法创建网页缓存目录");
    pruneSnapshots(m_cacheDir);
    const QString id = QUuid::createUuid().toString(QUuid::Id128);
    QSaveFile file(QDir(m_cacheDir).filePath(id + ".json"));
    const QByteArray bytes = QJsonDocument(snapshot).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return fail("CACHE_ERROR", "无法保存网页快照");
    return paginate(snapshot, id, 0);
}
