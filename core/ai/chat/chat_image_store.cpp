#include "chat_image_store.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QSaveFile>
#include <QUuid>

namespace {
using ImagesResult = Result<QJsonArray, DomainError>;
constexpr qint64 maximumBytes = 2 * 1024 * 1024;
constexpr auto ownerFile = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
DomainError invalidImage() { return domainError("CHAT_IMAGE_INVALID", QStringLiteral("请选择有效的 JPEG 或 PNG 图片，一次最多 4 张，每张不超过 2 MiB。")); }
DomainError storageError() { return domainError("STORE_UNAVAILABLE", QStringLiteral("图片无法保存或读取，请重试。")); }
Result<QByteArray, DomainError> readImage(const QString& path) {
    using R=Result<QByteArray,DomainError>;
    const QFileInfo info(path);
    if (!info.isAbsolute() || !info.isFile() || info.size() <= 0 || info.size() > maximumBytes)
        return R::failure(invalidImage());
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return R::failure(storageError());
    const auto bytes=file.read(maximumBytes+1);
    if (file.error()!=QFileDevice::NoError) return R::failure(storageError());
    if (bytes.isEmpty() || bytes.size()>maximumBytes) return R::failure(invalidImage());
    const auto mime=QMimeDatabase().mimeTypeForData(bytes).name();
    if (mime!="image/png" && mime!="image/jpeg") return R::failure(invalidImage());
    return R::success(bytes);
}
}
bool ChatImageStore::ownsFile(const QString& path) const {
    const QFileInfo info(path);
    const QString root=QFileInfo(m_directory).canonicalFilePath();
    return !root.isEmpty() && info.isAbsolute() && info.isFile() && !info.isSymLink()
        && info.canonicalPath()==root;
}
ImagesResult ChatImageStore::importImages(const QJsonArray& sources) const {
    if (sources.isEmpty() || sources.size()>4) return ImagesResult::failure(invalidImage());
    if (m_directory.isEmpty() || !QDir().mkpath(m_directory)) return ImagesResult::failure(storageError());
    QFile::setPermissions(m_directory, ownerFile | QFileDevice::ExeOwner);
    QJsonArray saved;
    auto fail=[&](const DomainError& error) { discardImages(saved); return ImagesResult::failure(error); };
    for (const auto& source:sources) {
        const auto data=readImage(source.toObject()["path"].toString());
        if (!data) return fail(data.error());
        const QString mime=QMimeDatabase().mimeTypeForData(data.value()).name();
        const QString id=QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString path=QDir(m_directory).absoluteFilePath(id+(mime=="image/png"?".png":".jpg"));
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(ownerFile)
            || file.write(data.value())!=data.value().size() || !file.commit()) return fail(storageError());
        QString name=QFileInfo(source.toObject()["name"].toString()).fileName().left(255);
        if (name.isEmpty()) name=QStringLiteral("图片")+(mime=="image/png"?".png":".jpg");
        saved.append(QJsonObject{{"id",id},{"name",name},{"mimeType",mime},{"path",path}});
    }
    return ImagesResult::success(saved);
}
ImagesResult ChatImageStore::contentBlocks(const QJsonArray& images) const {
    if (images.isEmpty() || images.size()>4) return ImagesResult::failure(invalidImage());
    QJsonArray blocks;
    for (const auto& value:images) {
        const auto image=value.toObject(); const QString path=image["path"].toString();
        if (!ownsFile(path)) return ImagesResult::failure(storageError());
        const auto data=readImage(path); if (!data) return ImagesResult::failure(data.error());
        const QString mime=QMimeDatabase().mimeTypeForData(data.value()).name();
        blocks.append(QJsonObject{{"type","image"},{"mediaType",mime},{"data",QString::fromLatin1(data.value().toBase64())}});
    }
    return ImagesResult::success(blocks);
}
void ChatImageStore::discardImages(const QJsonArray& images) const {
    for (const auto& image:images) {
        const auto path=image.toObject()["path"].toString();
        if (ownsFile(path)) QFile::remove(path);
    }
}
