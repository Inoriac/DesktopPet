#include "memory_policy.h"

#include "memory_store.h"

namespace {

bool textMatches(const MemoryEntry& entry, const QString& query) {
    const QString trimmed = query.trimmed();
    if (trimmed.isEmpty()) return false;
    for (const QString& tag : entry.tags) {
        if (tag.contains(trimmed, Qt::CaseInsensitive)) return true;
    }
    return entry.key.contains(trimmed, Qt::CaseInsensitive)
        || entry.summary.contains(trimmed, Qt::CaseInsensitive)
        || entry.content.contains(trimmed, Qt::CaseInsensitive);
}

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

bool isSentimentOpposite(const QString& summaryA, const QString& summaryB) {
    static const QList<QPair<QString, QString>> opposites = {
        {QStringLiteral("喜欢"), QStringLiteral("不喜欢")},
        {QStringLiteral("希望"), QStringLiteral("不希望")},
        {QStringLiteral("喜欢"), QStringLiteral("讨厌")},
        {QStringLiteral("想要"), QStringLiteral("不想要")},
        {QStringLiteral("喜欢"), QStringLiteral("不希望")},
        {QStringLiteral("希望"), QStringLiteral("不喜欢")},
    };

    for (const auto& pair : opposites) {
        if ((summaryA.contains(pair.first) && summaryB.contains(pair.second))
            || (summaryA.contains(pair.second) && summaryB.contains(pair.first))) {
            return true;
        }
    }
    return false;
}

bool isFirstOfScopeAndType(const MemoryStore* store, const MemoryEntry& entry) {
    for (const MemoryEntry& existing : store->all()) {
        if (existing.id == entry.id) continue;
        if (existing.status != MemoryStatus::Active) continue;
        if (existing.type == entry.type && existing.scope == entry.scope) return false;
    }
    return true;
}

}

MemoryPolicyReport MemoryPolicy::applyCandidates(const QList<MemoryCandidate>& candidates,
                                                 MemoryStore* store) const {
    MemoryPolicyReport report;
    if (!store) {
        report.skipped = candidates.size();
        report.notes.append(QStringLiteral("MemoryStore 未配置"));
        return report;
    }

    bool changed = false;
    QList<MemoryEntry> writtenEntries;

    for (const MemoryCandidate& candidate : candidates) {
        if (candidate.operation == MemoryCandidateOperation::Forget) {
            bool matched = false;
            for (const MemoryEntry& entry : store->all()) {
                if (!textMatches(entry, candidate.query)) continue;
                QJsonObject payloadPatch;
                payloadPatch["forget_query"] = candidate.query;
                payloadPatch["forget_source"] = candidate.rawText;
                if (store->updateStatusByKey(entry.type, entry.key, MemoryStatus::Deleted, payloadPatch)) {
                    matched = true;
                    changed = true;
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

        const MemoryEntry stored = store->addEntry(toWrite);
        changed = true;
        ++report.written;
        writtenEntries.append(stored);

        discoverRelations(stored, store, &report);
    }

    if (writtenEntries.size() >= 2) {
        discoverMentionedWith(writtenEntries, store, &report);
    }

    if (changed) {
        QString error;
        if (!store->save(&error)) {
            report.notes.append(QStringLiteral("保存记忆失败：%1").arg(error));
        }
    }

    return report;
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
                                      MemoryPolicyReport* report) const {
    MemoryRelationGraph& graph = store->relationGraph();

    for (const MemoryEntry& existing : store->all()) {
        if (existing.id == newEntry.id) continue;
        if (existing.status != MemoryStatus::Active) continue;

        // Supersedes: same type + same key
        if (existing.type == newEntry.type && existing.key == newEntry.key) {
            MemoryRelation rel;
            rel.fromMemoryId = newEntry.id;
            rel.toMemoryId = existing.id;
            rel.type = MemoryRelationType::Supersedes;
            rel.weight = 1.0;
            if (graph.addRelation(rel)) {
                ++report->relationsCreated;
            }
            store->updateStatusByKey(existing.type, existing.key, MemoryStatus::Superseded);
            continue;
        }

        // ConflictsWith: same scope, sentiment opposite
        if (!existing.scope.isEmpty() && existing.scope == newEntry.scope
            && isSentimentOpposite(newEntry.summary, existing.summary)) {
            if (!graph.hasRelation(newEntry.id, existing.id, MemoryRelationType::ConflictsWith)) {
                MemoryRelation rel;
                rel.fromMemoryId = newEntry.id;
                rel.toMemoryId = existing.id;
                rel.type = MemoryRelationType::ConflictsWith;
                rel.weight = 0.8;
                if (graph.addRelation(rel)) {
                    ++report->relationsCreated;
                }
            }
        }

        // Related: shared tags >= 2
        if (countSharedTags(newEntry.tags, existing.tags) >= 2) {
            if (!graph.hasRelation(newEntry.id, existing.id, MemoryRelationType::Related)
                && !graph.hasRelation(existing.id, newEntry.id, MemoryRelationType::Related)) {
                MemoryRelation rel;
                rel.fromMemoryId = newEntry.id;
                rel.toMemoryId = existing.id;
                rel.type = MemoryRelationType::Related;
                rel.weight = 0.6;
                if (graph.addRelation(rel)) {
                    ++report->relationsCreated;
                }
            }
        }
    }

    // DerivedFrom: sourceMemoryIds
    for (const QString& sourceId : newEntry.sourceMemoryIds) {
        if (sourceId.isEmpty()) continue;
        MemoryRelation rel;
        rel.fromMemoryId = newEntry.id;
        rel.toMemoryId = sourceId;
        rel.type = MemoryRelationType::DerivedFrom;
        rel.weight = 1.0;
        if (graph.addRelation(rel)) {
            ++report->relationsCreated;
        }
    }
}

void MemoryPolicy::discoverMentionedWith(const QList<MemoryEntry>& writtenEntries,
                                          MemoryStore* store,
                                          MemoryPolicyReport* report) const {
    MemoryRelationGraph& graph = store->relationGraph();

    for (int i = 0; i < writtenEntries.size(); ++i) {
        for (int j = i + 1; j < writtenEntries.size(); ++j) {
            MemoryRelation rel;
            rel.fromMemoryId = writtenEntries[i].id;
            rel.toMemoryId = writtenEntries[j].id;
            rel.type = MemoryRelationType::MentionedWith;
            rel.weight = 0.5;
            if (graph.addRelation(rel)) {
                ++report->relationsCreated;
            }
        }
    }
}
