#ifndef DESKTOP_PET_CHAT_IMAGE_STORE_H
#define DESKTOP_PET_CHAT_IMAGE_STORE_H
#include "ai/domain/domain_result.h"
#include <QJsonArray>

// A profile-owned directory. Call file operations from the persistence/read worker.
class ChatImageStore {
public:
    explicit ChatImageStore(QString directory) : m_directory(std::move(directory)) {}
    Result<QJsonArray, DomainError> importImages(const QJsonArray& sources) const;
    Result<QJsonArray, DomainError> contentBlocks(const QJsonArray& images) const;
    void discardImages(const QJsonArray& images) const;
private:
    bool ownsFile(const QString& path) const;
    QString m_directory;
};
#endif
