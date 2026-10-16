#ifndef DESKTOP_PET_MEMORY_ATTRIBUTION_TYPES_H
#define DESKTOP_PET_MEMORY_ATTRIBUTION_TYPES_H
#include "ai/context/context_spec.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <algorithm>

enum class MemoryAudienceKind { OwnerOnly, Conversation, Shareable, Internal, Invalid };
enum class MemoryClaimKind { Fact, Reported, Inferred };
enum class MemoryPersonRole { Subject, Participant };
struct MemoryAudience {
    MemoryAudienceKind kind = MemoryAudienceKind::OwnerOnly;
    QStringList conversationIds;
    bool isValid() const {
        if (kind == MemoryAudienceKind::Invalid)
            return false;
        if (kind == MemoryAudienceKind::Conversation && conversationIds.isEmpty())
            return false;
        return std::all_of(conversationIds.cbegin(), conversationIds.cend(),
                           [](const QString& id) { return !id.trimmed().isEmpty(); });
    }
    QJsonObject toJson() const {
        auto ids = conversationIds;
        ids.removeDuplicates();
        ids.sort();
        return {{"kind", kind == MemoryAudienceKind::Conversation ? "conversation"
                         : kind == MemoryAudienceKind::Shareable  ? "shareable"
                         : kind == MemoryAudienceKind::Internal   ? "internal"
                         : kind == MemoryAudienceKind::OwnerOnly  ? "owner_only"
                                                                  : "invalid"},
                {"conversationIds", QJsonArray::fromStringList(ids)}};
    }
    static MemoryAudience fromJson(const QJsonObject& j) {
        MemoryAudience a;
        const auto kind = j["kind"].toString();
        a.kind = kind == "conversation" ? MemoryAudienceKind::Conversation
                 : kind == "shareable"  ? MemoryAudienceKind::Shareable
                 : kind == "internal"   ? MemoryAudienceKind::Internal
                 : kind == "owner_only" ? MemoryAudienceKind::OwnerOnly
                                        : MemoryAudienceKind::Invalid;
        if (!j["conversationIds"].isArray())
            a.kind = MemoryAudienceKind::Invalid;
        for (const auto& id : j["conversationIds"].toArray()) {
            if (!id.isString() || id.toString().trimmed().isEmpty())
                a.kind = MemoryAudienceKind::Invalid;
            else
                a.conversationIds.append(id.toString());
        }
        if (!a.isValid())
            a.kind = MemoryAudienceKind::Invalid;
        return a;
    }
};
struct MemoryPersonReference {
    QString personId;
    MemoryPersonRole role = MemoryPersonRole::Subject;
};
struct MemoryOrigin {
    QString originId;
    QString conversationId;
    QString speakerPersonId;
    QDateTime originalAt;
    QString evidenceRef;
    bool committed = false;
};
struct MemoryAttribution {
    QString profileId;
    bool resolved = false;
    MemoryClaimKind claimKind = MemoryClaimKind::Reported;
    MemoryAudience audience;
    QList<MemoryPersonReference> people;
    QList<MemoryOrigin> origins;
    qint64 revision = 1;
    bool present() const {
        return !profileId.isEmpty();
    }
    QStringList subjects() const {
        QStringList result;
        for (const auto& p : people)
            if (p.role == MemoryPersonRole::Subject && !p.personId.isEmpty())
                result.append(p.personId);
        result.removeDuplicates();
        result.sort();
        return result;
    }
    QJsonObject toJson() const {
        QJsonArray persons, sources;
        auto sortedPeople = people;
        std::sort(sortedPeople.begin(), sortedPeople.end(), [](const auto& a, const auto& b) {
            return a.personId == b.personId ? int(a.role) < int(b.role) : a.personId < b.personId;
        });
        for (const auto& p : sortedPeople)
            persons.append(QJsonObject{
                {"personId", p.personId},
                {"role", p.role == MemoryPersonRole::Subject ? "subject" : "participant"}});
        auto sortedOrigins = origins;
        std::sort(sortedOrigins.begin(), sortedOrigins.end(),
                  [](const auto& a, const auto& b) { return a.originId < b.originId; });
        for (const auto& o : sortedOrigins)
            sources.append(
                QJsonObject{{"originId", o.originId},
                            {"conversationId", o.conversationId},
                            {"speakerPersonId", o.speakerPersonId},
                            {"originalAt", o.originalAt.isValid()
                                               ? o.originalAt.toUTC().toString(Qt::ISODateWithMs)
                                               : QString()},
                            {"evidenceRef", o.evidenceRef},
                            {"sourceStatus", o.committed ? "committed" : "pending"}});
        return {{"profileId", profileId},
                {"resolution", resolved ? "resolved" : "unresolved"},
                {"claimKind", claimKind == MemoryClaimKind::Fact       ? "fact"
                              : claimKind == MemoryClaimKind::Inferred ? "inferred"
                                                                       : "reported"},
                {"audience", audience.toJson()},
                {"people", persons},
                {"origins", sources},
                {"revision", revision}};
    }
    static MemoryAttribution fromJson(const QJsonObject& j) {
        MemoryAttribution a;
        a.profileId = j["profileId"].toString();
        a.resolved = j["resolution"].toString() == "resolved";
        const auto k = j["claimKind"].toString();
        a.claimKind = k == "fact"       ? MemoryClaimKind::Fact
                      : k == "inferred" ? MemoryClaimKind::Inferred
                                        : MemoryClaimKind::Reported;
        a.audience = MemoryAudience::fromJson(j["audience"].toObject());
        a.revision = qMax(qint64(1), j["revision"].toInteger(1));
        for (const auto& v : j["people"].toArray()) {
            auto p = v.toObject();
            a.people.append({p["personId"].toString(), p["role"].toString() == "participant"
                                                           ? MemoryPersonRole::Participant
                                                           : MemoryPersonRole::Subject});
        }
        for (const auto& v : j["origins"].toArray()) {
            auto o = v.toObject();
            a.origins.append({o["originId"].toString(), o["conversationId"].toString(),
                              o["speakerPersonId"].toString(),
                              QDateTime::fromString(o["originalAt"].toString(), Qt::ISODateWithMs),
                              o["evidenceRef"].toString(),
                              o["sourceStatus"].toString() == "committed"});
        }
        return a;
    }
};
struct AttributionContext {
    QString profileId, originId, speakerPersonId, conversationId;
    QDateTime occurredAt;
    MemoryClaimKind claimKind = MemoryClaimKind::Reported;
    MemoryAudience audience;
    bool resolved = false;
    bool inferSelfStatementClaim = false;
};
inline bool memoryVisibleInContext(const MemoryAttribution& a, const ContextSpec& c) {
    if (!a.present())
        return c.originKind == OriginKind::LocalOwner;
    if (a.profileId != c.profileId)
        return false;
    if (!a.audience.isValid())
        return false;
    switch (a.audience.kind) {
    case MemoryAudienceKind::Shareable:
        return true;
    case MemoryAudienceKind::OwnerOnly:
        return c.originKind == OriginKind::LocalOwner ||
               (c.originKind == OriginKind::Internal && c.allowOwnerMemory);
    case MemoryAudienceKind::Internal:
        return c.originKind == OriginKind::Internal && c.allowInternalMemory;
    case MemoryAudienceKind::Conversation:
        if (c.originKind != OriginKind::Internal)
            return !c.conversationId.isEmpty() &&
                   a.audience.conversationIds.contains(c.conversationId);
        for (const auto& id : c.authorizedConversationIds)
            if (a.audience.conversationIds.contains(id))
                return true;
        return false;
    case MemoryAudienceKind::Invalid:
        return false;
    }
    return false;
}
inline bool attributionSameSubjects(const MemoryAttribution& a, const MemoryAttribution& b) {
    if (!a.present() && !b.present())
        return true; // Legacy owner-only callers.
    return a.present() && b.present() && a.profileId == b.profileId && a.resolved && b.resolved &&
           !a.subjects().isEmpty() && a.subjects() == b.subjects();
}
inline bool attributionCanMerge(const MemoryAttribution& a, const MemoryAttribution& b) {
    if (!a.present() && !b.present())
        return true;
    if (!a.present() || !b.present() || a.profileId != b.profileId || a.claimKind != b.claimKind ||
        a.audience.toJson() != b.audience.toJson())
        return false;
    if (attributionSameSubjects(a, b))
        return true;
    if (a.resolved && b.resolved)
        return false;
    for (const auto& x : a.origins)
        for (const auto& y : b.origins)
            if (!x.originId.isEmpty() && x.originId == y.originId)
                return true;
    return false;
}
inline void mergeAttributionOrigins(MemoryAttribution& target, const MemoryAttribution& source) {
    if (!target.present()) {
        target = source;
        return;
    }
    if (target.profileId != source.profileId)
        return;
    for (const auto& person : source.people) {
        const bool present =
            std::any_of(target.people.cbegin(), target.people.cend(), [&](const auto& p) {
                return p.personId == person.personId && p.role == person.role;
            });
        if (!present)
            target.people.append(person);
    }
    for (const auto& o : source.origins) {
        auto found = std::find_if(target.origins.begin(), target.origins.end(),
                                  [&](const auto& x) { return x.originId == o.originId; });
        if (found == target.origins.end())
            target.origins.append(o);
        // A merge cannot turn pending evidence into committed proof. Only verified ingest can.
    }
}
#endif
