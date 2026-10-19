#include <QtTest>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QUrlQuery>
#include "core/ai/tools/web_content_service.h"

namespace {
const QString source = QStringLiteral("https://example.com/article");
WebHttpResponse response(const QByteArray& body, const QByteArray& type = "text/html; charset=utf-8") {
    return {true, body, type, QUrl(source), {}, {}, 200};
}
QString xmlItem(const QString& title, const QString& link, const QString& description = {}) {
    return "<item><title>" + title.toHtmlEscaped() + "</title><link>" + link.toHtmlEscaped()
        + "</link><description>" + description.toHtmlEscaped() + "</description></item>";
}
QByteArray feed(const QString& items) { return ("<rss version=\"2.0\"><channel>" + items + "</channel></rss>").toUtf8(); }
bool validUnicode(const QString& text) {
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text[i].isHighSurrogate()) {
            if (++i == text.size() || !text[i].isLowSurrogate()) return false;
        } else if (text[i].isLowSurrogate()) return false;
    }
    return true;
}
void verifyBudget(const QJsonObject& data) {
    QVERIFY(QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact)).size() <= 3800);
    std::function<void(QJsonValue)> visit = [&](QJsonValue value) {
        if (value.isString()) { QVERIFY(value.toString().size() <= 1800); QVERIFY(validUnicode(value.toString())); }
        else if (value.isArray()) for (const auto& child : value.toArray()) visit(child);
        else if (value.isObject()) for (const auto& child : value.toObject()) visit(child);
    };
    visit(data);
}
}

class TestWebContent : public QObject {
    Q_OBJECT
private slots:
    void search_validRss_structuredResults();
    void search_emptyRss_emptySuccess();
    void search_queryEncoding_preservesSpecialCharacters();
    void search_invalidFeedAndNetwork_failure();
    void search_longUnicode_budgetAndValidLinks();
    void readPage_htmlAndText_readableContent();
    void readPage_unicodePagination_losslessSnapshot();
    void readPage_invalidOrExpiredCursor_failure();
    void readPage_contentAndEncodingErrors_explicitFailure();
    void readPage_cacheRetention_boundedAndDeletedCursorFails();
    void security_invalidUrlsAndArguments_rejected();
};

void TestWebContent::search_validRss_structuredResults() {
    QTemporaryDir cache;
    int requests = 0;
    QUrl requested;
    int deadline = 0;
    qint64 sizeLimit = 0;
    WebContentService service(cache.path(), [&](const QUrl& url, int timeout, qint64 maximum) {
        ++requests;
        requested = url; deadline = timeout; sizeLimit = maximum;
        return response(feed(xmlItem("<b>Qt</b> & docs", source, "Read <em>this</em> &amp; learn")
            + xmlItem("duplicate", source) + xmlItem("Other", "https://example.org/page")), "application/rss+xml");
    });
    const auto result = service.search("  Qt 文档  ", 2);
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QCOMPARE(requests, 1);
    QCOMPARE(requested.host(), "cn.bing.com");
    QCOMPARE(QUrlQuery(requested).queryItemValue("q"), "Qt 文档");
    QCOMPARE(QUrlQuery(requested).queryItemValue("format"), "rss");
    QCOMPARE(deadline, 15000);
    QCOMPARE(sizeLimit, 500 * 1024);
    QCOMPARE(result.data["query"].toString(), "Qt 文档");
    const auto entries = result.data["entries"].toArray();
    QCOMPARE(entries.size(), 2);
    QCOMPARE(entries[0].toObject()["title"].toString(), "Qt & docs");
    QCOMPARE(entries[0].toObject()["snippet"].toString(), "Read this & learn");
    QCOMPARE(entries[0].toObject()["url"].toString(), source);
    verifyBudget(result.data);
}

void TestWebContent::search_queryEncoding_preservesSpecialCharacters() {
    QUrl requested;
    WebContentService service({}, [&](const QUrl& url, int, qint64) { requested = url; return response(feed({}), "text/xml"); });
    const QString query = "C++ & Qt 中文";
    QVERIFY(service.search(query).success);
    QCOMPARE(QUrlQuery(requested).queryItemValue("q", QUrl::FullyDecoded), query);
    QVERIFY(requested.toEncoded().contains("C%2B%2B%20%26%20Qt%20"));
}

void TestWebContent::search_emptyRss_emptySuccess() {
    WebContentService service({}, [](const QUrl&, int, qint64) { return response(feed({}), "text/xml"); });
    const auto result = service.search("empty");
    QVERIFY(result.success);
    QVERIFY(result.data["entries"].toArray().isEmpty());
}

void TestWebContent::search_invalidFeedAndNetwork_failure() {
    for (const auto& body : {QByteArray("<html><body>Challenge</body></html>"), QByteArray("<rss><channel>"),
                             QByteArray("<rss/>"), QByteArray("<rss><item/></rss>")}) {
        WebContentService service({}, [body](const QUrl&, int, qint64) { return response(body, "text/xml"); });
        const auto result = service.search("query");
        QVERIFY(!result.success);
        QCOMPARE(result.errorCode, "INVALID_FEED");
    }
    WebContentService service({}, [](const QUrl&, int, qint64) {
        return WebHttpResponse{false, {}, {}, {}, "TIMEOUT", "network timed out", 0};
    });
    const auto result = service.search("query");
    QVERIFY(!result.success);
    QCOMPARE(result.errorCode, "TIMEOUT");
}

void TestWebContent::search_longUnicode_budgetAndValidLinks() {
    const QString emoji = QString::fromUtf8("🙂");
    QString items = xmlItem("too long", "https://example.org/" + QString(1100, 'a'));
    for (int i = 0; i < 9; ++i)
        items += xmlItem(emoji.repeated(200), "https://example.org/" + QString(750, 'a') + QString::number(i), emoji.repeated(200));
    items += xmlItem("small tail", "https://example.org/tail");
    WebContentService service({}, [items](const QUrl&, int, qint64) { return response(feed(items), "text/xml"); });
    const auto result = service.search("预算");
    QVERIFY(result.success);
    QVERIFY(result.data["limited"].toBool());
    const auto entries = result.data["entries"].toArray();
    QVERIFY(!entries.isEmpty());
    QVERIFY(entries.size() < 5);
    for (const auto& entry : entries) {
        const auto object = entry.toObject();
        QVERIFY(object["title"].toString().size() <= 160);
        QVERIFY(object["snippet"].toString().size() <= 240);
        QVERIFY(QRegularExpression("[0-8]$").match(object["url"].toString()).hasMatch());
    }
    verifyBudget(result.data);
}

void TestWebContent::readPage_htmlAndText_readableContent() {
    QTemporaryDir cache;
    const QByteArray html = "<html><head><title>Title &amp; source</title></head><body><nav>Menu</nav>"
        "<main><h1>Article</h1><main><p>Nested body</p></main><p>After nested &amp; useful</p>"
        "<script>secret()</script><style>.bad{}</style><aside>Advertisement</aside>"
        "<img src=\"file:///tmp/private-web-content\"><form>Login</form></main><footer>Footer</footer></body></html>";
    WebContentService service(cache.path(), [html](const QUrl&, int, qint64) { return response(html); });
    const auto result = service.readPage(source);
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QCOMPARE(result.data["title"].toString(), "Title & source");
    QCOMPARE(result.data["content"].toString(), "Article\nNested body\nAfter nested & useful");
    QCOMPARE(result.data["size"].toInt(), html.size());
    QVERIFY(!result.data["hasMore"].toBool());
    verifyBudget(result.data);
    WebContentService plain(cache.path(), [](const QUrl&, int, qint64) { return response("first\n\nsecond", "text/plain"); });
    QCOMPARE(plain.readPage(source).data["content"].toString(), "first\n\nsecond");
}

void TestWebContent::readPage_unicodePagination_losslessSnapshot() {
    QTemporaryDir cache;
    int requests = 0;
    const QString content = (QString(1799, 'a') + QString::fromUtf8("🙂\n\"\\")).repeated(6);
    WebContentService service(cache.path(), [&](const QUrl&, int, qint64) { ++requests; return response(content.toUtf8(), "text/plain"); });
    auto page = service.readPage(source);
    QVERIFY2(page.success, qPrintable(page.errorMessage));
    const QString firstCursor = page.data["nextCursor"].toString();
    QVERIFY(!firstCursor.isEmpty());
    QString reconstructed;
    int pages = 0;
    while (true) {
        QVERIFY(page.success);
        verifyBudget(page.data);
        reconstructed += page.data["content"].toString();
        ++pages;
        if (!page.data["hasMore"].toBool()) break;
        QVERIFY(pages < 100);
        WebContentService reopened(cache.path(), [](const QUrl&, int, qint64) { return WebHttpResponse{}; });
        page = reopened.readPage(source, page.data["nextCursor"].toString());
    }
    QCOMPARE(reconstructed, content);
    QCOMPARE(requests, 1);
    QVERIFY(service.readPage(source).success);
    const auto originalContinuation = service.readPage(source, firstCursor);
    QVERIFY(originalContinuation.success);
    QCOMPARE(requests, 2);
}

void TestWebContent::readPage_invalidOrExpiredCursor_failure() {
    QTemporaryDir cache;
    WebContentService service(cache.path(), [](const QUrl&, int, qint64) { return response(QByteArray(4000, 'x'), "text/plain"); });
    const auto first = service.readPage(source);
    QVERIFY(first.success);
    const auto cursor = first.data["nextCursor"].toString();
    for (const auto& bad : {QString("../../private"), QString(300, 'x')}) {
        const auto result = service.readPage(source, bad);
        QVERIFY(!result.success);
        QCOMPARE(result.errorCode, "INVALID_CURSOR");
    }
    QCOMPARE(service.readPage("https://example.org/other", cursor).errorCode, "INVALID_CURSOR");
    const auto files = QDir(cache.path()).entryList({"*.json"}, QDir::Files);
    QCOMPARE(files.size(), 1);
    QFile snapshot(QDir(cache.path()).filePath(files.first()));
    QVERIFY(snapshot.open(QIODevice::ReadOnly));
    auto data = QJsonDocument::fromJson(snapshot.readAll()).object();
    snapshot.close();
    data["createdAt"] = QDateTime::currentDateTimeUtc().addDays(-2).toString(Qt::ISODateWithMs);
    QVERIFY(snapshot.open(QIODevice::WriteOnly | QIODevice::Truncate));
    snapshot.write(QJsonDocument(data).toJson());
    snapshot.close();
    QCOMPARE(service.readPage(source, cursor).errorCode, "INVALID_CURSOR");
}

void TestWebContent::readPage_cacheRetention_boundedAndDeletedCursorFails() {
    QTemporaryDir cache;
    WebContentService service(cache.path(), [](const QUrl&, int, qint64) { return response(QByteArray(4000, 'x'), "text/plain"); });
    const auto oldest = service.readPage(source);
    QVERIFY(oldest.success);
    const QString oldCursor = oldest.data["nextCursor"].toString();
    for (int i = 0; i < 65; ++i) QVERIFY(service.readPage(source).success);
    QCOMPARE(QDir(cache.path()).entryList({"*.json"}, QDir::Files).size(), 64);
    QCOMPARE(service.readPage(source, oldCursor).errorCode, "INVALID_CURSOR");
    const auto latest = service.readPage(source);
    QVERIFY(latest.success);
    const auto latestCursor = latest.data["nextCursor"].toString();
    const QString snapshot = QDir(cache.path()).filePath(latestCursor.section(':', 0, 0) + ".json");
    QVERIFY(QFile::remove(snapshot));
    QCOMPARE(service.readPage(source, latestCursor).errorCode, "INVALID_CURSOR");
}

void TestWebContent::readPage_contentAndEncodingErrors_explicitFailure() {
    QTemporaryDir cache;
    const QList<QPair<WebHttpResponse, QString>> cases = {
        {response("%PDF", "application/pdf"), "UNSUPPORTED_CONTENT"},
        {response("<script>app()</script>"), "NO_READABLE_CONTENT"},
        {response("<p>abc</p>", "text/html; charset=x-unknown"), "UNSUPPORTED_ENCODING"},
        {response(QByteArray("abc\xff"), "text/plain; charset=utf-8"), "INVALID_ENCODING"},
        {WebHttpResponse{true, "not found", "text/plain", QUrl(source), {}, {}, 404}, "HTTP_ERROR"},
        {response(QByteArray(500 * 1024 + 1, 'a'), "text/plain"), "RESPONSE_TOO_LARGE"}
    };
    for (const auto& value : cases) {
        WebContentService service(cache.path(), [value](const QUrl&, int, qint64) { return value.first; });
        const auto result = service.readPage(source);
        QVERIFY(!result.success);
        QCOMPARE(result.errorCode, value.second);
    }
    WebContentService latin(cache.path(), [](const QUrl&, int, qint64) { return response(QByteArray("caf\xe9"), "text/plain; charset=ISO-8859-1"); });
    QCOMPARE(latin.readPage(source).data["content"].toString(), QString::fromUtf8("café"));
    QFile blocked(cache.filePath("file"));
    QVERIFY(blocked.open(QIODevice::WriteOnly));
    blocked.close();
    WebContentService unusable(blocked.fileName(), [](const QUrl&, int, qint64) { return response("readable", "text/plain"); });
    QCOMPARE(unusable.readPage(source).errorCode, "CACHE_ERROR");
}

void TestWebContent::security_invalidUrlsAndArguments_rejected() {
    int requests = 0;
    WebContentService service({}, [&](const QUrl&, int, qint64) { ++requests; return WebHttpResponse{}; });
    for (const auto& url : {QString("file:///etc/passwd"), QString("http://127.0.0.1/"), QString("https://user:password@example.com/"),
                            QString("http://localhost/"), QString("http://[::ffff:127.0.0.1]/"), source + QString(1100, 'x')}) {
        QVERIFY(!service.readPage(url).success);
    }
    QVERIFY(!service.search("  ").success);
    QVERIFY(!service.search(QString(201, 'x')).success);
    QVERIFY(!service.search("query", 0).success);
    QVERIFY(!service.search("query", 6).success);
    QVERIFY(!service.readPage(source, {}, 999).success);
    QVERIFY(!service.readPage(source, {}, 30001).success);
    QCOMPARE(requests, 0);
}

QTEST_GUILESS_MAIN(TestWebContent)
#include "test_web_content.moc"
