#include "memory_policy.h"

#include "memory_store.h"

namespace {
bool textMatches(const MemoryEntry& entry, const QString& query) {
    const QString trimmed = query.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }
    for (const QString& tag : entry.tags) {
        if (tag.contains(trimmed, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return entry.key.contains(trimmed, Qt::CaseInsensitive)
        || entry.summary.contains(trimmed, Qt::CaseInsensitive)
        || entry.content.contains(trimmed, Qt::CaseInsensitive);
}

bool hasDuplicateActiveMemory(const MemoryStore* store, const MemoryCandidate& candidate) {
    if (!store || candidate.operation != MemoryCandidateOperation::Write) {
        return false;
    }

    const MemoryEntry& target = candidate.entry;
    for (const MemoryEntry& entry : store->all()) {
        if (entry.status != MemoryStatus::Active) {
            continue;
        }
        if (entry.type == target.type && entry.key == target.key) {
            return true;
        }
        if (entry.type == target.type
            && !target.summary.trimmed().isEmpty()
            && entry.summary.compare(target.summary, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
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
    for (const MemoryCandidate& candidate : candidates) {
        if (candidate.operation == MemoryCandidateOperation::Forget) {
            bool matched = false;
            for (const MemoryEntry& entry : store->all()) {
                if (!textMatches(entry, candidate.query)) {
                    continue;
                }
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
            if (!reason.isEmpty()) {
                report.notes.append(reason);
            }
            continue;
        }

        if (hasDuplicateActiveMemory(store, candidate)) {
            ++report.skipped;
            report.notes.append(QStringLiteral("跳过重复记忆：%1").arg(candidate.entry.summary));
            continue;
        }

        store->addEntry(candidate.entry);
        changed = true;
        ++report.written;
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
    if (candidate.operation != MemoryCandidateOperation::Write) {
        return true;
    }

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
