#ifndef DESKTOP_PET_PROFILE_CHAT_HISTORY_STORE_H
#define DESKTOP_PET_PROFILE_CHAT_HISTORY_STORE_H

#include "ai/chat/chat_types.h"

#include <QList>
#include <QString>
#include <QStringList>

struct ProfileChatStoreOptions {
    QString appDataRoot;
    QString profileId;
    QStringList registeredProfileIds;
    QString legacyHistoryPath;
};

class ProfileChatHistoryStore {
public:
    bool open(const ProfileChatStoreOptions& options, QString* errorMessage);
    QList<ChatHistoryEntry> load(QString* errorMessage) const;
    bool appendFinal(const ChatHistoryEntry& entry, QString* errorMessage);

private:
    bool importLegacyIfEligible(const ProfileChatStoreOptions& options,
                                QString* errorMessage);
    bool ensureHistoryFile(QString* errorMessage) const;

    QString m_historyPath;
    QString m_profileId;
    bool m_open = false;
};

#endif // DESKTOP_PET_PROFILE_CHAT_HISTORY_STORE_H
