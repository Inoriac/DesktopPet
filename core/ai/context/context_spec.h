#ifndef DESKTOP_PET_CONTEXT_SPEC_H
#define DESKTOP_PET_CONTEXT_SPEC_H
#include "ai/identity/identity_types.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>

// Host-owned scope. Model proposals must never fill its authorization projection.
struct ContextSpec {
    QString profileId;
    QString channel;
    QString conversationId;
    QString activityId;
    QStringList participantPersonIds;
    OriginKind originKind = OriginKind::Internal;
    qint64 conversationRevision = 0;
    qint64 identityRevision = 0;
    QString audienceProjectionRef;
    bool allowOwnerMemory = false;
    QStringList authorizedConversationIds;
    bool allowInternalMemory = false;

    QString cacheKey() const {
        auto ids = authorizedConversationIds;
        ids.removeDuplicates();
        ids.sort();
        return QString::fromUtf8(
            QJsonDocument(QJsonArray{profileId, conversationId,
                                     conversationId.isEmpty() ? activityId : QString(),
                                     int(originKind), audienceProjectionRef, allowOwnerMemory,
                                     allowInternalMemory, QJsonArray::fromStringList(ids)})
                .toJson(QJsonDocument::Compact));
    }
    static ContextSpec localOwner(const QString& profile) {
        ContextSpec c;
        c.profileId = profile;
        c.channel = "desktop";
        c.conversationId = "local_owner";
        c.originKind = OriginKind::LocalOwner;
        c.participantPersonIds = {"owner"};
        return c;
    }
};
#endif
