#include "model_downloader.h"

#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QSaveFile>

namespace {
QString safeRelativePath(const QString& path) {
    const QString normalized = QDir::cleanPath(QDir::fromNativeSeparators(path.trimmed()));
    if (normalized.isEmpty()
        || normalized == "."
        || QDir::isAbsolutePath(normalized)
        || normalized == ".."
        || normalized.startsWith("../")) {
        return {};
    }
    return normalized;
}

bool pathIsWithin(const QString& path, const QString& root) {
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseSensitive;
#endif
    const QString rootPrefix = root.endsWith('/') ? root : root + '/';
    return path.compare(root, sensitivity) == 0 || path.startsWith(rootPrefix, sensitivity);
}
}

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
void ModelDownloader::setMaxFileBytes(qint64 bytes) { m_maxFileBytes = qMax<qint64>(1, bytes); }

QUrl ModelDownloader::buildUrl(const QString& mirrorHost, const QString& repo, const QString& file) const {
    // {mirror}/{repo}/resolve/{revision}/{file}
    QString url = mirrorHost;
    if (!url.endsWith(QLatin1Char('/'))) url += QLatin1Char('/');
    url += repo + QStringLiteral("/resolve/") + m_revision + QStringLiteral("/") + file;
    return QUrl(url);
}

bool ModelDownloader::verifySha256(const QString& path, const QString& expected) {
    if (!QFileInfo(path).isFile()) return false;
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
    QString canonicalRoot = dir.canonicalPath();
    canonicalRoot.replace('\\', '/');
    if (canonicalRoot.isEmpty()) {
        if (errorMessage) *errorMessage = QStringLiteral("无法解析模型目录: %1").arg(destDir);
        emit finished(false);
        return false;
    }

    bool allOk = true;
    for (const FileSpec& spec : files) {
        const QString relativePath = safeRelativePath(spec.relativePath);
        if (relativePath.isEmpty()) {
            allOk = false;
            emit fileFinished(spec.relativePath, false, QStringLiteral("非法的模型相对路径"));
            continue;
        }
        const QString destPath = dir.filePath(relativePath);
        // 确保子目录存在（如 onnx/model.onnx 的 onnx 子目录）
        if (!QDir().mkpath(QFileInfo(destPath).absolutePath())) {
            allOk = false;
            emit fileFinished(relativePath, false, QStringLiteral("无法创建目标子目录"));
            continue;
        }
        QString canonicalParent = QFileInfo(destPath).absoluteDir().canonicalPath();
        canonicalParent.replace('\\', '/');
        if (!pathIsWithin(canonicalParent, canonicalRoot)) {
            allOk = false;
            emit fileFinished(relativePath, false, QStringLiteral("模型路径越出目标目录"));
            continue;
        }
        if (QFile::exists(destPath)) {
            QString canonicalDestination = QFileInfo(destPath).canonicalFilePath();
            canonicalDestination.replace('\\', '/');
            if (!pathIsWithin(canonicalDestination, canonicalRoot)) {
                allOk = false;
                emit fileFinished(relativePath, false, QStringLiteral("模型文件越出目标目录"));
                continue;
            }
            if (QFileInfo(destPath).size() > m_maxFileBytes) {
                allOk = false;
                emit fileFinished(relativePath, false, QStringLiteral("模型文件超过大小限制"));
                continue;
            }
            if (verifySha256(destPath, spec.sha256)) {
                emit fileFinished(spec.relativePath, true, QStringLiteral("已存在，跳过"));
                continue;
            }
        }

        QString note;
        FileSpec safeSpec = spec;
        safeSpec.relativePath = relativePath;
        const bool ok = downloadFile(repo, destPath, safeSpec, &note);
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
            bool oversized = false;
            QObject::connect(&timeoutTimer, &QTimer::timeout, [&]() {
                timedOut = true;
                if (reply) reply->abort();
            });
            QObject::connect(reply, &QNetworkReply::downloadProgress, reply,
                             [this, reply, &oversized, relativePath = spec.relativePath]
                             (qint64 received, qint64 total) {
                if (received > m_maxFileBytes || (total > 0 && total > m_maxFileBytes)) {
                    oversized = true;
                    reply->abort();
                    return;
                }
                emit progress(relativePath, received, total);
            });
            QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            timeoutTimer.start(m_timeoutMs);
            loop.exec();
            timeoutTimer.stop();

            if (oversized) {
                if (note) *note = QStringLiteral("Downloaded file exceeds size limit");
                reply->deleteLater();
                continue;
            }
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
            QSaveFile out(destPath);
            if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                if (note) *note = QStringLiteral("无法写入: %1").arg(destPath);
                reply->deleteLater();
                continue;
            }
            const QByteArray bytes = reply->readAll();
            if (bytes.size() > m_maxFileBytes) {
                if (note) *note = QStringLiteral("Downloaded file exceeds size limit");
                out.cancelWriting();
                reply->deleteLater();
                continue;
            }
            if (out.write(bytes) != bytes.size() || !out.commit()) {
                if (note) *note = QStringLiteral("写入失败: %1").arg(destPath);
                reply->deleteLater();
                continue;
            }
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
