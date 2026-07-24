#ifndef DESKTOP_PET_MODEL_DOWNLOADER_H
#define DESKTOP_PET_MODEL_DOWNLOADER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

class QNetworkAccessManager;

// 模型下载器：从 HuggingFace（镜像优先）下载 Embedding 模型文件到本地目录。
//
// 设计要点（针对国内 HF 直连不稳）：
//   - 镜像优先：hf-mirror.com 为主，huggingface.co 为备。镜像顺序可配置。
//   - 逐镜像重试：每个镜像超时/失败自动切下一个，每镜像可重试 N 次。
//   - 校验：可选 sha256 校验，不匹配则删除重下。
//   - 降级：全部失败返回 false，调用方应降级为 Noop（不阻塞主功能），下次启动重试。
//   - 已存在且校验通过则跳过下载。
//
// 同步封装：模型下载发生在初始化期，故提供 downloadSync() 阻塞至完成；
// 也可走 downloadAsync() + finished 信号异步下载。
class ModelDownloader : public QObject {
    Q_OBJECT
public:
    struct Mirror {
        QString host;        // 例 "https://hf-mirror.com"
        QString resolvePath; // 例 "/{repo}/resolve/{revision}/{file}"
    };

    struct FileSpec {
        QString relativePath; // 相对模型目录的文件名，如 "onnx/model.onnx"
        QString sha256;       // 可选校验和；空则跳过校验
    };

    explicit ModelDownloader(QObject* parent = nullptr);

    void setMirrors(const QStringList& hosts);   // 默认内置 hf-mirror + huggingface
    void setRevision(const QString& revision);   // 默认 "main"
    void setRetriesPerMirror(int n);             // 默认 2
    void setTransferTimeoutMs(int ms);           // 默认 30000

    // 同步下载 repo 下所有 FileSpec 到 destDir，返回是否全部成功。
    // repo 例 "BAAI/bge-small-zh-v1.5"；destDir 不存在则创建。单个文件失败不中断其余。
    bool downloadSync(const QString& repo, const QString& destDir,
                      const QList<FileSpec>& files, QString* errorMessage = nullptr);

signals:
    void progress(const QString& relativePath, qint64 received, qint64 total);
    void fileFinished(const QString& relativePath, bool ok, const QString& note);
    void finished(bool allOk);

private:
    QNetworkAccessManager* m_manager;
    QStringList m_mirrors;
    QString m_revision = QStringLiteral("main");
    int m_retries = 2;
    int m_timeoutMs = 30000;

    QUrl buildUrl(const QString& mirrorHost, const QString& repo, const QString& file) const;
    bool downloadFile(const QString& repo, const QString& destPath, const FileSpec& spec, QString* note);
    static bool verifySha256(const QString& path, const QString& expected);
};

#endif // DESKTOP_PET_MODEL_DOWNLOADER_H