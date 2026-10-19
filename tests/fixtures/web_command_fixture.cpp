#include "plugins/web/web_command.h"
#include <QGuiApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QUrlQuery>
#include <cstdio>

int main(int argc, char** argv) {
    QCoreApplication::setAttribute(Qt::AA_PluginApplication);
    QGuiApplication application(argc, argv);
    const auto arguments = application.arguments().mid(1);
    const int stateIndex = arguments.indexOf("--state-dir");
    const QString stateDir = stateIndex >= 0 ? arguments.value(stateIndex + 1) : QString();
    WebHttpGet transport = [stateDir](const QUrl& url, int, qint64) {
        // Only this test executable records requests or serves controlled pages.
        if (!stateDir.isEmpty() && QDir().mkpath(stateDir)) {
            QFile log(QDir(stateDir).filePath("backend-calls.jsonl"));
            if (log.open(QIODevice::WriteOnly | QIODevice::Append))
                log.write(QJsonDocument(QJsonObject{{"url", url.toString()}}).toJson(QJsonDocument::Compact) + '\n');
        }
        const auto query = QUrlQuery(url).queryItemValue("q", QUrl::FullyDecoded);
        if (query == "network-failure" || url.path() == "/network-failure")
            return WebHttpResponse{false, {}, {}, url, "NETWORK_ERROR", "Controlled network failure", 0};
        if (url.host() == "cn.bing.com") {
            const QString title = query == "long-results" ? QString::fromUtf8("搜索🙂").repeated(100) : "Qt documentation";
            const QString snippet = query == "long-results" ? QString::fromUtf8("摘要🙂").repeated(100) : "Read the official material";
            QString items;
            for (int i = 0; i < 5; ++i) {
                const QString link = "https://example.com/article" + (i ? QString::number(i) : QString());
                items += "<item><title>" + title.toHtmlEscaped() + "</title><link>" + link
                    + "</link><description>" + snippet.toHtmlEscaped() + "</description></item>";
            }
            return WebHttpResponse{true, ("<rss version=\"2.0\"><channel>" + items + "</channel></rss>").toUtf8(),
                                   "application/rss+xml", url, {}, {}, 200};
        }
        if (url.path() == "/code") return WebHttpResponse{true,
            "<main><h1>Tutorial</h1><p>Use <code>print()</code> now.</p><pre>print('hello')</pre></main>",
            "text/html", url, {}, {}, 200};
        if (url.path() == "/empty") return WebHttpResponse{true, "<script>app()</script>", "text/html", url, {}, {}, 200};
        const QString content = QString::fromUtf8("网页🙂 \"\\\n").repeated(900);
        return WebHttpResponse{true, content.toUtf8(), "text/plain; charset=utf-8", url, {}, {}, 200};
    };
    QFile input, output;
    if (!input.open(stdin, QIODevice::ReadOnly) || !output.open(stdout, QIODevice::WriteOnly)) return 1;
    return runWebCommand(arguments, input, output, std::move(transport));
}
