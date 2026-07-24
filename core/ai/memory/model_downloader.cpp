#include "model_downloader.h"

#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

ModelDownloader::ModelDownloader(QObject* parent)
    : QObject(parent), m_manager(new QNetworkAccessManager(this)) {
    // 默认镜像：国内镜像优先，huggingface 官方兜底。
    m_mirrors = {
        QStringLiteral("https://hf-mirror.com"),
        QStringLiteral("https://huggingface.co"),
    };
}

void ModelDownloader::setMirrors(const QStringList& hosts) { m_mirrors = hosts; }
void ModelDownloader::setRevision(const QString& revision) { m_revision = revision; }
void ModelDownloader::setRetriesPerMirror(int n) { m_retries = qMax(0, n); }
void ModelDownloader::setTransferTimeoutMs(int ms) { m_timeoutMs = qMax(1000, ms); }

QUrl ModelDownloader::buildUrl(const QString& mirrorHost, const QString& repo, const QString& file) const {
    // {mirror}/{repo}/resolve/{revision}/{file}
    QString url = mirrorHost;
    if (!url.endsWith(QLatin1Char('/'))) url += QLatin1Char('/');
    url += repo + QStringLiteral("/resolve/") + m_revision + QStringLiteral("/") + file;
    return QUrl(url);
}

bool ModelDownloader::verifySha256(const QString& path, const QString& expected) {
    if (expected.isEmpty()) return true;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&f)) return false;
    return QString::fromLatin1(hash.result().toHex()).compare(expected, Qt::CaseInsensitive) == 0;
}

bool ModelDownloader::downloadSync(const QString& repo, const QString& destDir,
                                   const QList<FileSpec>& files, QString* errorMessage) {
    QDir dir(destDir);
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        if (errorMessage) *errorMessage = QStringLiteral("无法创建模型目录: %1").arg(destDir);
        emit finished(false);
        return false;
    }

    bool allOk = true;
    for (const FileSpec& spec : files) {
        const QString destPath = dir.filePath(spec.relativePath);
        // 已存在且校验通过 → 跳过
        if (QFile::exists(destPath) && verifySha256(destPath, spec.sha256)) {
            emit fileFinished(spec.relativePath, true, QStringLiteral("已存在，跳过"));
            continue;
        }
        // 确保子目录存在（如 onnx/model.onnx 的 onnx 子目录）
        QDir().mkpath(QFileInfo(destPath).absolutePath());

        QString note;
        const bool ok = downloadFile(repo, destPath, spec, &note);
        if (!ok) allOk = false;
        emit fileFinished(spec.relativePath, ok, note);
    }

    if (!allOk && errorMessage) {
        *errorMessage = QStringLiteral("部分模型文件下载失败（见 fileFinished 信号）");
    }
    emit finished(allOk);
    return allOk;
}

bool ModelDownloader::downloadFile(const QString& repo, const QString& destPath,
                                   const FileSpec& spec, QString* note) {
    for (const QString& mirror : m_mirrors) {
        for (int attempt = 0; attempt <= m_retries; ++attempt) {
            const QUrl url = buildUrl(mirror, repo, spec.relativePath);
            if (!url.isValid()) continue;

            QNetworkRequest request(url);
            request.setHeader(QNetworkRequest::UserAgentHeader,
                              QStringLiteral("DesktopPet-ModelDownloader/1.0"));
            request.setTransferTimeout(m_timeoutMs);

            QNetworkReply* reply = m_manager->get(request);
            QEventLoop loop;
            QTimer timeoutTimer;
            timeoutTimer.setSingleShot(true);
            bool timedOut = false;
            QObject::connect(&timeoutTimer, &QTimer::timeout, [&]() {
                timedOut = true;
                if (reply) reply->abort();
            });
            QObject::connect(reply, &QNetworkReply::downloadProgress, this,
                             [this](qint64 r, qint64 t) { /* emit progress 可在此处转发 */ });
            QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            timeoutTimer.start(m_timeoutMs);
            loop.exec();
            timeoutTimer.stop();

            if (timedOut) {
                reply->deleteLater();
                continue;  // 超时 → 换/重试
            }
            if (reply->error() != QNetworkReply::NoError) {
                if (note) *note = reply->errorString();
                reply->deleteLater();
                continue;
            }

            // 写文件
            QFile out(destPath);
            if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                if (note) *note = QStringLiteral("无法写入: %1").arg(destPath);
                reply->deleteLater();
                continue;
            }
            out.write(reply->readAll());
            out.close();
            reply->deleteLater();

            // 校验
            if (!verifySha256(destPath, spec.sha256)) {
                QFile::remove(destPath);
                if (note) *note = QStringLiteral("sha256 校验失败");
                continue;
            }
            if (note) *note = QStringLiteral("已下载");
            return true;
        }
    }
    return false;
}