#include "ai/tools/music_tools.h"
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

namespace {
struct MusicServer {
    QTcpServer server;
    QList<QByteArray> targets;
    int status = 200;
    bool hold = false;
    MusicServer() {
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            auto *socket = server.nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer = QByteArray{}]() mutable {
                buffer += socket->readAll();
                if (!buffer.contains("\r\n\r\n")) return;
                targets.append(buffer.split(' ').value(1));
                socket->disconnect(socket, &QTcpSocket::readyRead, nullptr, nullptr);
                if (hold) return;
                const QByteArray body = status == 200 ? "{\"status\":\"playing\"}" : "service error";
                socket->write("HTTP/1.1 " + QByteArray::number(status) + " Result\r\nContent-Length: "
                              + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        });
    }
    QUrl url() const { return QUrl(QString("http://127.0.0.1:%1").arg(server.serverPort())); }
};
}

class NativeMusicTests : public QObject {
    Q_OBJECT
private slots:
    void failed_requests_are_not_retried_or_replaced_with_startup_data_data() {
        QTest::addColumn<QString>("path");
        for (const auto *path : {"/status", "/lyric", "/play", "/pause", "/skip-next", "/skip-prev", "/volume"})
            QTest::newRow(path) << QString::fromLatin1(path);
    }
    void failed_requests_are_not_retried_or_replaced_with_startup_data() {
        QFETCH(QString, path);
        MusicServer fixture;
        QVERIFY(fixture.server.listen(QHostAddress::LocalHost));
        fixture.status = 503;
        const auto result = callLxMusicApi(path, {}, 1000, fixture.url());
        QVERIFY(!result.success);
        QVERIFY(result.errorMessage.contains("503"));
        QCOMPARE(fixture.targets, QList<QByteArray>{path.toUtf8()});
    }
    void timed_out_control_is_sent_once() {
        MusicServer fixture;
        QVERIFY(fixture.server.listen(QHostAddress::LocalHost));
        fixture.hold = true;
        const auto result = callLxMusicApi("/skip-next", {}, 100, fixture.url());
        QVERIFY(!result.success);
        QCOMPARE(result.errorMessage, "lx_music_api_timeout");
        QCOMPARE(fixture.targets, QList<QByteArray>{"/skip-next"});
    }
    void successful_request_preserves_query_and_result() {
        MusicServer fixture;
        QVERIFY(fixture.server.listen(QHostAddress::LocalHost));
        QUrlQuery query;
        query.addQueryItem("volume", "27");
        const auto result = callLxMusicApi("/volume", query, 1000, fixture.url());
        QVERIFY(result.success);
        QCOMPARE(result.data["status"].toString(), "playing");
        QCOMPARE(result.data["api_path"].toString(), "/volume");
        QCOMPARE(fixture.targets, QList<QByteArray>{"/volume?volume=27"});
    }
    void polling_budget_includes_http_wait_data() {
        QTest::addColumn<bool>("playback");
        QTest::newRow("startup") << false;
        QTest::newRow("playback") << true;
    }
    void polling_budget_includes_http_wait() {
        QFETCH(bool, playback);
        MusicServer fixture;
        QVERIFY(fixture.server.listen(QHostAddress::LocalHost));
        fixture.hold = true;
        QElapsedTimer elapsed;
        elapsed.start();
        const auto result = playback ? waitForLxMusicPlayback({}, {}, 250, fixture.url())
                                     : waitForLxMusicApiReady(250, fixture.url());
        QVERIFY(!result.success);
        // Allow scheduling jitter, but never wait a full 1200 ms HTTP timeout.
        QVERIFY2(elapsed.elapsed() < 1000, qPrintable(QString::number(elapsed.elapsed())));
        QVERIFY(!fixture.targets.isEmpty());
    }
};
QTEST_GUILESS_MAIN(NativeMusicTests)
#include "test_native_music.moc"
