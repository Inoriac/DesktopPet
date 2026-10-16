#include "memory_attribution_service.h"
#include "associative_activation_engine.h"
#include "hippocampus_working_set.h"
#include "memory_keyword_index.h"
#include "memory_metadata.h"
#include "memory_policy.h"
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <utility>
namespace {
DomainError unavailable() {
    return domainError("STORE_UNAVAILABLE", "Memory attribution store is unavailable");
}
bool selfStatement(const QString& text) {
    static const QRegularExpression pattern(
        QStringLiteral("^(?:请)?(?:帮我)?(?:记住[：:，, "
                       "]*)?我(?:喜欢|不喜欢|讨厌|希望|不希望|想要|不想要|叫)[^\\r\\n]*$"));
    const auto trimmed = text.trimmed();
    if (pattern.match(trimmed).hasMatch())
        return true;
    // Only recognizable own activities establish a first-person subject. A temporal
    // prefix alone ("我今天听甲说乙…") does not establish whose fact was reported.
    static const QRegularExpression activity(
        QStringLiteral("^我(?:(?:最近|今天|昨天|明天|这周|上周|周末))?"
                       "(?:(?:正在|在)?(?:学习|练习|阅读|工作|画画|散步|跑步|游泳|旅行)"
                       "|在[^，。！？\\r\\n]{1,24}(?:散步|跑步|游泳|工作|学习|练习))"
                       "[^，。！？\\r\\n]*$"));
    static const QRegularExpression reported(QStringLiteral("听.*说|告诉|提到|表示|认为|觉得"));
    return activity.match(trimmed).hasMatch() && !reported.match(trimmed).hasMatch();
}
} // namespace
MemoryAttributionService::MemoryAttributionService(QString profile, MemoryStore* store,
                                                   SourceLookup source, PersonLookup person)
    : m_profileId(std::move(profile)), m_store(store), m_sourceLookup(std::move(source)),
      m_personLookup(std::move(person)) {
}
Result<MemoryEntry, DomainError>
MemoryAttributionService::attribute(const AttributedMemoryInput& input) const {
    using R = Result<MemoryEntry, DomainError>;
    const auto& c = input.attribution;
    if (m_profileId.isEmpty() || c.profileId != m_profileId)
        return R::failure(domainError("INVALID_INPUT", "Attribution profile mismatch"));
    if (!c.audience.isValid())
        return R::failure(domainError("INVALID_INPUT", "Invalid attribution audience"));
    if (c.audience.kind == MemoryAudienceKind::Conversation && c.audience.conversationIds.isEmpty())
        return R::failure(
            domainError("INVALID_INPUT", "Conversation audience requires explicit conversations"));
    MemoryEntry entry = input.entry;
    entry.attribution = {};
    auto& a = entry.attribution;
    a.profileId = m_profileId;
    a.audience = c.audience;
    a.claimKind = c.claimKind;
    if (c.originId.isEmpty())
        return R::success(entry);
    std::optional<MemoryOriginSource> source;
    if (m_sourceLookup) {
        auto result = m_sourceLookup(m_profileId, c.originId);
        if (!result)
            return R::failure(result.error());
        source = result.value();
    }
    MemoryOrigin origin;
    origin.originId = c.originId;
    origin.evidenceRef = c.originId;
    origin.conversationId = c.conversationId;
    if (!source) {
        a.origins.append(origin);
        return R::success(entry);
    }
    if (source->profileId != m_profileId || source->originId != c.originId ||
        (!c.speakerPersonId.isEmpty() && source->speakerPersonId != c.speakerPersonId) ||
        (!c.conversationId.isEmpty() && source->conversationId != c.conversationId))
        return R::failure(
            domainError("UNKNOWN_REFERENCE", "Original event does not match attribution"));
    auto known = [&](const QString& person) -> Result<bool, DomainError> {
        if (person.isEmpty() || !m_personLookup)
            return Result<bool, DomainError>::success(false);
        return m_personLookup(m_profileId, person);
    };
    origin.committed = true;
    origin.originalAt = source->occurredAt;
    origin.conversationId = source->conversationId;
    auto speaker = known(source->speakerPersonId);
    if (!speaker)
        return R::failure(speaker.error());
    if (speaker.value())
        origin.speakerPersonId = source->speakerPersonId;
    a.origins.append(origin);
    for (const auto& p : input.describedPeople) {
        const bool firstPerson = p.role == MemoryPersonRole::Subject &&
                                 p.personId == origin.speakerPersonId &&
                                 selfStatement(source->text);
        const bool sourceReference =
            std::any_of(source->resolvedPeople.cbegin(), source->resolvedPeople.cend(),
                        [&](const auto& reference) {
                            return reference.personId == p.personId && reference.role == p.role;
                        });
        if (!firstPerson && !sourceReference)
            return R::failure(domainError(
                "UNKNOWN_REFERENCE", "Described person is not an original resolved reference"));
        auto exists = known(p.personId);
        if (!exists)
            return R::failure(exists.error());
        if (!exists.value())
            return R::failure(
                domainError("UNKNOWN_REFERENCE", "Described person is not in this profile"));
        a.people.append(p);
    }
    if (a.subjects().isEmpty() && !origin.speakerPersonId.isEmpty() && selfStatement(source->text))
        a.people.append({origin.speakerPersonId, MemoryPersonRole::Subject});
    a.resolved = !a.subjects().isEmpty();
    if (c.inferSelfStatementClaim && !origin.speakerPersonId.isEmpty() &&
        selfStatement(source->text))
        a.claimKind = MemoryClaimKind::Fact;
    entry.lastMentionedAt = source->occurredAt;
    return R::success(entry);
}
Result<MemoryWriteResult, DomainError>
MemoryAttributionService::ingestMemory(const AttributedMemoryInput& input) {
    using R = Result<MemoryWriteResult, DomainError>;
    if (!m_store || !m_store->databaseIsOpen())
        return R::failure(unavailable());
    auto resolved = attribute(input);
    if (!resolved)
        return R::failure(resolved.error());
    MemoryCandidate candidate;
    candidate.entry = resolved.value();
    candidate.explicitRequest = input.explicitRequest;
    if (!MemoryPolicy().shouldAutoWrite(candidate))
        return R::success({});
    if (!m_store->beginTransaction())
        return R::failure(unavailable());
    auto fail = [&]() {
        m_store->rollbackTransaction();
        m_store->refreshDatabaseOnly();
        return R::failure(unavailable());
    };
    QSqlQuery lock(QSqlDatabase::database(m_store->databaseConnectionName()));
    if (!lock.exec("UPDATE memory_attribution SET revision=revision WHERE 0") ||
        !m_store->refreshDatabaseOnly())
        return fail();
    MemoryEntry entry = resolved.value();
    MemoryWriteResult result;
    MemoryMutationBatch mutations;
    std::optional<MemoryEntry> duplicate;
    for (const auto& old : m_store->all()) {
        if (old.status != MemoryStatus::Active || !MemoryMetadata::sameProposition(old, entry))
            continue;
        const bool repairsPendingSource =
            !old.attribution.resolved && old.attribution.people.isEmpty() &&
            old.attribution.profileId == entry.attribution.profileId &&
            old.attribution.audience.toJson() == entry.attribution.audience.toJson() &&
            std::any_of(old.attribution.origins.cbegin(), old.attribution.origins.cend(),
                        [&](const auto& pending) {
                            return !pending.committed &&
                                   std::any_of(entry.attribution.origins.cbegin(),
                                               entry.attribution.origins.cend(),
                                               [&](const auto& committed) {
                                                   return committed.committed &&
                                                          committed.originId == pending.originId;
                                               });
                        });
        if (attributionCanMerge(old.attribution, entry.attribution) || repairsPendingSource) {
            duplicate = old;
            break;
        }
    }
    if (duplicate) {
        MemoryEntry updated = *duplicate;
        const auto previous = updated.attribution.toJson();
        if (!updated.attribution.resolved && entry.attribution.resolved) {
            auto origins = updated.attribution;
            updated.attribution = entry.attribution;
            mergeAttributionOrigins(updated.attribution, origins);
        } else
            mergeAttributionOrigins(updated.attribution, entry.attribution);
        // Replace only the source just validated; generic metadata rebases cannot promote pending.
        for (const auto& original : entry.attribution.origins)
            for (auto& saved : updated.attribution.origins)
                if (saved.originId == original.originId && original.committed)
                    saved = original;
        if (previous != updated.attribution.toJson()) {
            updated.attribution.revision = duplicate->attribution.revision + 1;
            MemoryMetadata::mergeContext(updated, entry);
            updated.mentionCount =
                qMax(1, updated.mentionCount) +
                qMax(0, updated.attribution.origins.size() - duplicate->attribution.origins.size());
            if (!m_store->stageEntryUpdate(updated, &mutations))
                return fail();
        }
        result.memoryIds.append(updated.id);
        result.merged = 1;
    } else {
        auto staged = MemoryPolicy().stageCandidates({candidate}, m_store);
        mutations = staged.mutations;
        result.written = staged.report.written;
        for (const auto& mutation : mutations.entries)
            if (!mutation.before)
                result.memoryIds.append(mutation.after.id);
    }
    result.unresolved = entry.attribution.resolved ? 0 : 1;
    if (!m_store->persistMutationBatch(mutations) || !m_store->commitTransaction())
        return fail();
    return R::success(result);
}
Result<QList<RetrievedMemory>, DomainError>
MemoryAttributionService::recallForContext(const ScopedMemoryQuery& query) {
    using R = Result<QList<RetrievedMemory>, DomainError>;
    if (!m_store || !m_store->databaseIsOpen())
        return R::failure(unavailable());
    if (query.context.profileId != m_profileId)
        return R::failure(domainError("INVALID_INPUT", "Recall profile mismatch"));
    if (!m_store->refreshDatabaseOnly())
        return R::failure(unavailable());
    MemoryQuery q;
    q.text = query.text;
    q.limit = query.limit;
    q.context = query.context;
    q.subjectPersonIds = query.subjectPersonIds;
    HippocampusWorkingSet ws(m_store);
    ws.refresh();
    MemoryKeywordIndex keywords;
    keywords.rebuild(m_store->all());
    AssociativeActivationEngine graph;
    ActivationChannels channels;
    channels.workingSet = &ws;
    channels.keywordIndex = &keywords;
    channels.graphPropagation = &graph;
    const auto memories = MemoryRetriever().retrieveWithGraphPropagation(*m_store, q, channels);
    if (!m_store->lastReadSucceeded())
        return R::failure(unavailable());
    return R::success(memories);
}
Result<MigrationProgress, DomainError>
MemoryAttributionService::migrateAttribution(const AttributionMigrationRequest& request) {
    using R = Result<MigrationProgress, DomainError>;
    if (!m_store || !m_store->databaseIsOpen())
        return R::failure(unavailable());
    if (request.profileId != m_profileId || request.batchSize < 1 || request.batchSize > 1000)
        return R::failure(domainError("INVALID_INPUT", "Invalid migration request"));
    if (!m_store->beginTransaction())
        return R::failure(unavailable());
    auto fail = [&](DomainError e) {
        m_store->rollbackTransaction();
        return R::failure(e);
    };
    QSqlQuery q(QSqlDatabase::database(m_store->databaseConnectionName()));
    if (!q.exec("UPDATE memory_attribution SET revision=revision WHERE 0"))
        return fail(unavailable());
    q.prepare("SELECT cursor,completed FROM agent_attribution_migrations WHERE profile_id=?");
    q.addBindValue(m_profileId);
    if (!q.exec())
        return fail(unavailable());
    MigrationProgress progress;
    if (q.next()) {
        progress.nextCursor = q.value(0).toString();
        progress.complete = q.value(1).toBool();
    }
    q.finish();
    if (!request.cursor.isEmpty() && request.cursor != progress.nextCursor)
        return fail(domainError("STALE_REVISION", "Migration cursor has advanced"));
    if (progress.complete) {
        if (!m_store->commitTransaction())
            return fail(unavailable());
        return R::success(progress);
    }
    // Hold the writer transaction while checking for recovery dependencies.
    // Maintenance batches are replayed before calling us; if a new one appears,
    // defer it too. Retry leases contain no pending mutation and are safe to skip.
    if (!q.exec("SELECT 1 FROM sleep_staged_change WHERE status='Prepared' AND NOT "
                "(session_id='maintenance-retry' AND target_type='daydream_retry') LIMIT 1"))
        return fail(unavailable());
    if (q.next()) {
        progress.deferredForRecovery = true;
        q.finish();
        if (!m_store->commitTransaction())
            return fail(unavailable());
        return R::success(progress);
    }
    q.finish();
    q.prepare("SELECT id FROM memory_items WHERE id>? ORDER BY id LIMIT ?");
    q.addBindValue(progress.nextCursor.isNull() ? QStringLiteral("") : progress.nextCursor);
    q.addBindValue(request.batchSize + 1);
    if (!q.exec())
        return fail(unavailable());
    QStringList ids;
    while (q.next())
        ids.append(q.value(0).toString());
    q.finish();
    progress.complete = ids.size() <= request.batchSize;
    if (!progress.complete)
        ids.removeLast();
    for (const auto& id : ids) {
        auto old = m_store->readForRecall(id);
        if (!old)
            return fail(unavailable());
        if (!old->attribution.present()) {
            AttributedMemoryInput input;
            input.entry = *old;
            input.attribution.profileId = m_profileId;
            input.attribution.originId = old->payload.value("event_id").toString();
            auto entry = attribute(input);
            if (!entry)
                return fail(entry.error());
            // Migration only changes associations, never the original body or clocks.
            if (!m_store->writeAttributionOnly(id, entry.value().attribution))
                return fail(unavailable());
            if (!entry.value().attribution.resolved)
                ++progress.unresolved;
        }
        progress.nextCursor = id;
        ++progress.processed;
    }
    q.prepare("INSERT INTO agent_attribution_migrations(profile_id,cursor,completed) VALUES(?,?,?) "
              "ON CONFLICT(profile_id) DO UPDATE SET "
              "cursor=excluded.cursor,completed=excluded.completed,updated_at=strftime('%Y-%m-%dT%"
              "H:%M:%fZ','now')");
    q.addBindValue(m_profileId);
    q.addBindValue(progress.nextCursor);
    q.addBindValue(progress.complete);
    if (!q.exec() || !m_store->commitTransaction())
        return fail(unavailable());
    m_store->refreshDatabaseOnly();
    return R::success(progress);
}
