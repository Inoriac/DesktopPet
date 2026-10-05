#include "memory_policy.h"

#include <QCryptographicHash>
#include <QRegularExpression>

#include <optional>

#include "memory_metadata.h"
#include "memory_store.h"

namespace {

bool hasDuplicateActiveMemory(const MemoryStore* store, const MemoryCandidate& candidate) {
    if (!store || candidate.operation != MemoryCandidateOperation::Write) return false;

    const MemoryEntry& target = candidate.entry;
    for (const MemoryEntry& entry : store->all()) {
        if (entry.status != MemoryStatus::Active) continue;
        if (entry.type == target.type
            && !target.summary.trimmed().isEmpty()
            && entry.summary.compare(target.summary, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

int countSharedTags(const QStringList& tagsA, const QStringList& tagsB) {
    int count = 0;
    for (const QString& tag : tagsA) {
        if (tagsB.contains(tag, Qt::CaseInsensitive)) ++count;
    }
    return count;
}

struct PreferenceAssertion {
    QString subject;
    QString attribute;
    QString object;
    bool positive = false;
};

std::optional<PreferenceAssertion> parsePreferenceAssertion(QString text,
                                                            bool implicitUser) {
    text = text.trimmed();
    if (implicitUser) {
        static const QRegularExpression rememberPrefix(QStringLiteral(
            "^(?:请)?(?:帮我)?(?:记住|记一下|记下来)[：:，, ]*"));
        text.remove(rememberPrefix);
    }
    text.remove(QRegularExpression(QStringLiteral("[。.!！]+$")));
    static const QRegularExpression statement(QStringLiteral(
        "^(我|用户)?\\s*(不喜欢|讨厌|不希望|不想要|喜欢|希望|想要)[：:，, ]*(.+)$"));
    const QRegularExpressionMatch match = statement.match(text);
    if (!match.hasMatch() || (match.captured(1).isEmpty() && !implicitUser)) {
        return std::nullopt;
    }

    const QString object = match.captured(3).simplified().toCaseFolded();
    // Only assert a conflict for a complete, unambiguous proposition. Compound
    // clauses, alternatives and uncertainty need model review, not a rule edge.
    static const QRegularExpression ambiguousObject(QStringLiteral(
        "[，,。.!！?？；;：:\\r\\n]|但是|不过|然而|如果|是否|是不是|也许|可能|据说|"
        "或者|还是|喜欢|讨厌|希望|想要"));
    if (object.isEmpty() || ambiguousObject.match(object).hasMatch()) {
        return std::nullopt;
    }

    const QString predicate = match.captured(2);
    PreferenceAssertion assertion;
    assertion.subject = QStringLiteral("user");
    assertion.object = object;
    assertion.positive = !predicate.startsWith(QStringLiteral("不"))
        && predicate != QStringLiteral("讨厌");
    if (predicate.endsWith(QStringLiteral("喜欢")) || predicate == QStringLiteral("讨厌")) {
        assertion.attribute = QStringLiteral("likes");
    } else if (predicate.endsWith(QStringLiteral("希望"))) {
        assertion.attribute = QStringLiteral("hopes");
    } else {
        assertion.attribute = QStringLiteral("wants");
    }
    return assertion;
}

std::optional<PreferenceAssertion> preferenceAssertion(const MemoryEntry& entry) {
    // rule_v1 rewrites both dislikes and negative wishes as "用户不希望...".
    // Its original evidence preserves which property the user actually stated.
    if (entry.payload.value(QStringLiteral("extractor")).toString() == QStringLiteral("rule_v1")) {
        if (entry.evidence.size() != 1) return std::nullopt;
        return parsePreferenceAssertion(entry.evidence.first(), true);
    }
    return parsePreferenceAssertion(
        entry.content.trimmed().isEmpty() ? entry.summary : entry.content, false);
}

bool hasOppositeAssertion(const MemoryEntry& a, const MemoryEntry& b) {
    const auto assertionA = preferenceAssertion(a);
    const auto assertionB = preferenceAssertion(b);
    return assertionA && assertionB
        && assertionA->subject == assertionB->subject
        && assertionA->attribute == assertionB->attribute
        && assertionA->object == assertionB->object
        && assertionA->positive != assertionB->positive;
}

bool isFirstOfScopeAndType(const MemoryStore* store, const MemoryEntry& entry) {
    for (const MemoryEntry& existing : store->all()) {
        if (existing.id == entry.id) continue;
        if (existing.status != MemoryStatus::Active) continue;
        if (existing.type == entry.type && existing.scope == entry.scope) return false;
    }
    return true;
}

MemoryRelation stagedRelation(const MemoryEntry& from,
                              const MemoryEntry& to,
                              MemoryRelationType type,
                              double weight) {
    MemoryRelation relation;
    relation.fromMemoryId = from.id;
    relation.toMemoryId = to.id;
    relation.type = type;
    relation.weight = weight;
    relation.createdAt = QDateTime::currentDateTimeUtc();
    const QByteArray identity = QStringLiteral("%1\n%2\n%3")
        .arg(relation.fromMemoryId, relation.toMemoryId,
             memoryRelationTypeToString(type)).toUtf8();
    relation.id = QString::fromLatin1(
        QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
    return relation;
}

}

bool MemoryPolicy::matchesForgetQuery(const MemoryEntry& entry,
                                      const QString& query) {
    const QString trimmed = query.trimmed();
    if (trimmed.isEmpty()) return false;
    for (const QString& tag : entry.tags) {
        if (tag.contains(trimmed, Qt::CaseInsensitive)) return true;
    }
    return entry.key.contains(trimmed, Qt::CaseInsensitive)
        || entry.summary.contains(trimmed, Qt::CaseInsensitive)
        || entry.content.contains(trimmed, Qt::CaseInsensitive);
}

MemoryPolicyReport MemoryPolicy::applyCandidates(const QList<MemoryCandidate>& candidates,
                                                 MemoryStore* store) const {
    StagedMemoryPolicyResult staged = stageCandidates(candidates, store);
    if (!store) return staged.report;
    if (!store->persistMutationBatch(staged.mutations)) {
        store->rollbackMutationBatch(staged.mutations);
        staged.report.notes.append(QStringLiteral("保存记忆失败"));
        return staged.report;
    }
    if (!staged.mutations.entries.isEmpty()) {
        QString error;
        if (!store->save(&error)) {
            staged.report.notes.append(QStringLiteral("保存记忆失败：%1").arg(error));
        }
    }
    return staged.report;
}

StagedMemoryPolicyResult MemoryPolicy::stageCandidates(
    const QList<MemoryCandidate>& candidates,
    MemoryStore* store) const {
    StagedMemoryPolicyResult staged;
    MemoryPolicyReport& report = staged.report;
    if (!store) {
        report.skipped = candidates.size();
        report.notes.append(QStringLiteral("MemoryStore 未配置"));
        return staged;
    }

    QList<MemoryEntry> writtenEntries;

    for (const MemoryCandidate& candidate : candidates) {
        if (candidate.operation == MemoryCandidateOperation::Forget) {
            bool matched = false;
            for (const MemoryEntry& entry : store->all()) {
                if (!matchesForgetQuery(entry, candidate.query)) continue;
                MemoryEntry updated = entry;
                updated.status = MemoryStatus::Deleted;
                updated.updatedAt = QDateTime::currentDateTimeUtc();
                updated.payload[QStringLiteral("forget_query")] = candidate.query;
                updated.payload[QStringLiteral("forget_source")] = candidate.rawText;
                if (store->stageEntryUpdate(updated, &staged.mutations)) {
                    matched = true;
                }
            }
            if (matched) {
                ++report.forgotten;
            } else {
                ++report.skipped;
                report.notes.append(QStringLiteral("未找到匹配的可删除记忆：%1").arg(candidate.query));
            }
            continue;
        }

        QString reason;
        if (!shouldAutoWrite(candidate, &reason)) {
            ++report.skipped;
            if (!reason.isEmpty()) report.notes.append(reason);
            continue;
        }

        if (hasDuplicateActiveMemory(store, candidate)) {
            ++report.skipped;
            report.notes.append(QStringLiteral("跳过重复记忆：%1").arg(candidate.entry.summary));
            continue;
        }

        MemoryEntry toWrite = candidate.entry;

        if (isFirstOfScopeAndType(store, toWrite)) {
            toWrite.importance = qMin(1.0, toWrite.importance + 0.15);
        }

        const MemoryEntry stored = store->stageEntry(toWrite, &staged.mutations);
        ++report.written;
        writtenEntries.append(stored);

        discoverRelations(stored, store, &report, &staged.mutations);
    }

    if (writtenEntries.size() >= 2) {
        discoverMentionedWith(writtenEntries, &report, &staged.mutations);
    }

    return staged;
}

bool MemoryPolicy::shouldAutoWrite(const MemoryCandidate& candidate, QString* reason) const {
    if (candidate.operation != MemoryCandidateOperation::Write) return true;

    const MemoryEntry& entry = candidate.entry;
    if (entry.summary.trimmed().isEmpty() && entry.content.trimmed().isEmpty()) {
        if (reason) *reason = QStringLiteral("候选记忆为空，跳过");
        return false;
    }

    if (entry.privacyLevel == PrivacyLevel::Sensitive) {
        if (reason) *reason = QStringLiteral("候选记忆可能包含敏感信息，需要确认后再保存：%1").arg(entry.summary);
        return false;
    }

    if (!candidate.explicitRequest && entry.confidence < 0.8) {
        if (reason) *reason = QStringLiteral("非显式记忆置信度不足，跳过：%1").arg(entry.summary);
        return false;
    }

    return true;
}

void MemoryPolicy::discoverRelations(const MemoryEntry& newEntry,
                                      MemoryStore* store,
                                      MemoryPolicyReport* report,
                                      MemoryMutationBatch* mutations) const {
    for (const MemoryEntry& existing : store->all()) {
        if (existing.id == newEntry.id) continue;
        if (existing.status != MemoryStatus::Active) continue;

        // Supersedes: same type + same key
        if (existing.type == newEntry.type && existing.key == newEntry.key) {
            mutations->relations.append(stagedRelation(
                newEntry, existing, MemoryRelationType::Supersedes, 1.0));
            ++report->relationsCreated;
            MemoryEntry superseded = existing;
            superseded.status = MemoryStatus::Superseded;
            superseded.updatedAt = QDateTime::currentDateTimeUtc();
            superseded.payload[QStringLiteral("superseded_by")] = newEntry.id;
            store->stageEntryUpdate(superseded, mutations);
            continue;
        }

        // ConflictsWith requires the same proposition with opposite polarity.
        if (!existing.scope.isEmpty() && existing.scope == newEntry.scope
            && hasOppositeAssertion(newEntry, existing)) {
            mutations->relations.append(stagedRelation(
                newEntry, existing, MemoryRelationType::ConflictsWith, 0.8));
            ++report->relationsCreated;
        }

        // Source and routing tags do not establish a shared topic.
        if (countSharedTags(MemoryMetadata::semanticTags(newEntry),
                            MemoryMetadata::semanticTags(existing)) >= 2) {
            mutations->relations.append(stagedRelation(
                newEntry, existing, MemoryRelationType::Related, 0.6));
            ++report->relationsCreated;
        }
    }

    // DerivedFrom: sourceMemoryIds
    for (const QString& sourceId : newEntry.sourceMemoryIds) {
        if (sourceId.isEmpty()) continue;
        MemoryEntry source;
        source.id = sourceId;
        mutations->relations.append(stagedRelation(
            newEntry, source, MemoryRelationType::DerivedFrom, 1.0));
        ++report->relationsCreated;
    }
}

void MemoryPolicy::discoverMentionedWith(const QList<MemoryEntry>& writtenEntries,
                                          MemoryPolicyReport* report,
                                          MemoryMutationBatch* mutations) const {
    for (int i = 0; i < writtenEntries.size(); ++i) {
        for (int j = i + 1; j < writtenEntries.size(); ++j) {
            mutations->relations.append(stagedRelation(
                writtenEntries[i], writtenEntries[j],
                MemoryRelationType::MentionedWith, 0.5));
            ++report->relationsCreated;
        }
    }
}
