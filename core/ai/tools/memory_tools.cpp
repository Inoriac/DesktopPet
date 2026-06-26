//
// Memory maintenance tools
//

#include "memory_tools.h"

#include "ai/memory/memory_relation.h"
#include "ai/memory/memory_store.h"

#include <algorithm>
#include <cmath>

#include <QHash>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QtGlobal>

namespace {

constexpr int kDefaultMaxChanges = 25;
constexpr int kMaxChangesLimit = 100;
constexpr int kAutoCooldownSecs = 30 * 60;

struct MemoryOrganizeStats {
    QString mode;
    bool dryRun = false;
    bool skippedCooldown = false;
    int scanned = 0;
    int changed = 0;
    int expired = 0;
    int archived = 0;
    int superseded = 0;
    int canonicalUpdated = 0;
    int metadataCompacted = 0;
    int relationsCreated = 0;
    int skippedSensitive = 0;
    bool snapshotSaved = false;
    QString snapshotError;

    QJsonObject toJson() const {
        QJsonObject obj;
        obj[QStringLiteral("mode")] = mode;
        obj[QStringLiteral("dry_run")] = dryRun;
        obj[QStringLiteral("skipped_cooldown")] = skippedCooldown;
        obj[QStringLiteral("scanned")] = scanned;
        obj[QStringLiteral("changed")] = changed;
        obj[QStringLiteral("expired")] = expired;
        obj[QStringLiteral("archived")] = archived;
        obj[QStringLiteral("superseded")] = superseded;
        obj[QStringLiteral("canonical_updated")] = canonicalUpdated;
        obj[QStringLiteral("metadata_compacted")] = metadataCompacted;
        obj[QStringLiteral("relations_created")] = relationsCreated;
        obj[QStringLiteral("skipped_sensitive")] = skippedSensitive;
        obj[QStringLiteral("snapshot_saved")] = snapshotSaved;
        if (!snapshotError.isEmpty()) {
            obj[QStringLiteral("snapshot_error")] = snapshotError;
        }
        obj[QStringLiteral("note")] = QStringLiteral("No memories were physically deleted.");
        return obj;
    }
};

QJsonObject makeStringProperty(const QString& description, const QString& defaultValue = {}) {
    QJsonObject obj;
    obj[QStringLiteral("type")] = QStringLiteral("string");
    obj[QStringLiteral("description")] = description;
    if (!defaultValue.isEmpty()) {
        obj[QStringLiteral("default")] = defaultValue;
    }
    return obj;
}

QJsonObject makeIntegerProperty(const QString& description, int defaultValue) {
    QJsonObject obj;
    obj[QStringLiteral("type")] = QStringLiteral("integer");
    obj[QStringLiteral("description")] = description;
    obj[QStringLiteral("default")] = defaultValue;
    return obj;
}

bool isAllowedMode(const QString& mode) {
    static const QStringList modes = {
        QStringLiteral("auto"),
        QStringLiteral("report"),
        QStringLiteral("expire"),
        QStringLiteral("archive"),
        QStringLiteral("merge_duplicates"),
        QStringLiteral("compact"),
    };
    return modes.contains(mode);
}

bool modeIncludes(const QString& mode, const QString& pass) {
    return mode == QLatin1String("auto")
        || mode == QLatin1String("report")
        || mode == pass;
}

bool hasChangeBudget(const MemoryOrganizeStats& stats, int maxChanges) {
    return stats.changed < maxChanges;
}

void countChange(MemoryOrganizeStats* stats) {
    if (stats) {
        ++stats->changed;
    }
}

QString dateTimeText(const QDateTime& value) {
    return value.isValid() ? value.toUTC().toString(Qt::ISODate) : QString();
}

QStringList compactStringList(const QStringList& values) {
    QStringList result;
    QSet<QString> seen;
    for (const QString& value : values) {
        const QString trimmed = value.trimmed();
        if (trimmed.isEmpty()) continue;
        const QString key = trimmed.toLower();
        if (seen.contains(key)) continue;
        seen.insert(key);
        result.append(trimmed);
    }
    return result;
}

QStringList mergedStringList(QStringList base, const QStringList& extra) {
    base.append(extra);
    return compactStringList(base);
}

double clampedScore(double value) {
    return qBound(0.0, value, 1.0);
}

QString bestSummary(const MemoryEntry& entry) {
    if (!entry.summary.trimmed().isEmpty()) return entry.summary.trimmed();
    if (!entry.content.trimmed().isEmpty()) return entry.content.trimmed();
    return entry.key.trimmed();
}

QString normalizedText(const QString& text) {
    QString normalized = text.toLower().trimmed();
    normalized.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return normalized;
}

QString duplicateKeyFor(const MemoryEntry& entry) {
    const QString summary = normalizedText(bestSummary(entry));
    if (summary.isEmpty()) return {};
    return QStringLiteral("%1\n%2\n%3")
        .arg(memoryTypeToString(entry.type), entry.scope.trimmed().toLower(), summary);
}

bool isProtectedFromExpiry(const MemoryEntry& entry) {
    return entry.type == MemoryType::Core
        || entry.type == MemoryType::TaskShadow
        || entry.privacyLevel == PrivacyLevel::Sensitive;
}

bool isSensitive(const MemoryEntry& entry, QSet<QString>* skippedSensitiveIds) {
    if (entry.privacyLevel != PrivacyLevel::Sensitive) {
        return false;
    }
    if (skippedSensitiveIds) {
        skippedSensitiveIds->insert(entry.id);
    }
    return true;
}

QDateTime referenceTimeForAge(const MemoryEntry& entry) {
    if (entry.updatedAt.isValid()) return entry.updatedAt;
    if (entry.createdAt.isValid()) return entry.createdAt;
    return {};
}

bool isOlderThanDays(const MemoryEntry& entry, int days, const QDateTime& now) {
    const QDateTime reference = referenceTimeForAge(entry);
    if (!reference.isValid()) return false;
    return reference.daysTo(now) >= days;
}

bool shouldArchiveOperationalMemory(const MemoryEntry& entry,
                                    const QDateTime& now,
                                    int shortTermDays,
                                    int eventDays) {
    if (entry.status != MemoryStatus::Active) return false;
    if (entry.privacyLevel == PrivacyLevel::Sensitive) return false;

    if (entry.type == MemoryType::ShortTerm
        && entry.key == QLatin1String("assistant_response")) {
        return isOlderThanDays(entry, shortTermDays, now);
    }

    if (entry.type == MemoryType::Event
        && (entry.key == QLatin1String("tool_execution")
            || entry.tags.contains(QStringLiteral("tool_execution"), Qt::CaseInsensitive))) {
        return isOlderThanDays(entry, eventDays, now);
    }

    return false;
}

bool compactMetadata(MemoryEntry* entry) {
    if (!entry) return false;

    MemoryEntry updated = *entry;
    updated.tags = compactStringList(updated.tags);
    updated.evidence = compactStringList(updated.evidence);
    updated.sourceMemoryIds = compactStringList(updated.sourceMemoryIds);
    updated.supersedes = compactStringList(updated.supersedes);
    updated.conflictsWith = compactStringList(updated.conflictsWith);
    updated.importance = clampedScore(updated.importance);
    updated.strength = clampedScore(updated.strength);
    updated.confidence = clampedScore(updated.confidence);
    updated.emotionIntensity = clampedScore(updated.emotionIntensity);
    updated.emotionConfidence = clampedScore(updated.emotionConfidence);
    if (updated.summary.trimmed().isEmpty() && !updated.content.trimmed().isEmpty()) {
        updated.summary = updated.content.trimmed();
    }

    const bool changed = updated.tags != entry->tags
        || updated.evidence != entry->evidence
        || updated.sourceMemoryIds != entry->sourceMemoryIds
        || updated.supersedes != entry->supersedes
        || updated.conflictsWith != entry->conflictsWith
        || !qFuzzyCompare(updated.importance + 1.0, entry->importance + 1.0)
        || !qFuzzyCompare(updated.strength + 1.0, entry->strength + 1.0)
        || !qFuzzyCompare(updated.confidence + 1.0, entry->confidence + 1.0)
        || !qFuzzyCompare(updated.emotionIntensity + 1.0, entry->emotionIntensity + 1.0)
        || !qFuzzyCompare(updated.emotionConfidence + 1.0, entry->emotionConfidence + 1.0)
        || updated.summary != entry->summary;

    if (changed) {
        *entry = updated;
    }
    return changed;
}

bool canonicalLessPreferred(const MemoryEntry& a, const MemoryEntry& b) {
    if (std::abs(a.importance - b.importance) > 0.0001) return a.importance < b.importance;
    if (std::abs(a.confidence - b.confidence) > 0.0001) return a.confidence < b.confidence;
    if (a.accessCount != b.accessCount) return a.accessCount < b.accessCount;
    return a.updatedAt < b.updatedAt;
}

bool mergeDuplicateIntoCanonical(MemoryEntry* canonical, const MemoryEntry& duplicate) {
    if (!canonical) return false;

    MemoryEntry updated = *canonical;
    updated.tags = mergedStringList(updated.tags, duplicate.tags);
    updated.evidence = mergedStringList(updated.evidence, duplicate.evidence);
    updated.sourceMemoryIds.append(duplicate.sourceMemoryIds);
    updated.sourceMemoryIds.append(duplicate.id);
    updated.sourceMemoryIds = compactStringList(updated.sourceMemoryIds);
    updated.supersedes.append(duplicate.id);
    updated.supersedes = compactStringList(updated.supersedes);
    updated.importance = qMax(updated.importance, duplicate.importance);
    updated.strength = qMax(updated.strength, duplicate.strength);
    updated.confidence = qMax(updated.confidence, duplicate.confidence);
    updated.mentionCount += duplicate.mentionCount;
    updated.accessCount += duplicate.accessCount;
    updated.payload[QStringLiteral("merged_duplicate_count")] =
        updated.payload.value(QStringLiteral("merged_duplicate_count")).toInt(0) + 1;

    compactMetadata(&updated);

    const bool changed = updated.tags != canonical->tags
        || updated.evidence != canonical->evidence
        || updated.sourceMemoryIds != canonical->sourceMemoryIds
        || updated.supersedes != canonical->supersedes
        || !qFuzzyCompare(updated.importance + 1.0, canonical->importance + 1.0)
        || !qFuzzyCompare(updated.strength + 1.0, canonical->strength + 1.0)
        || !qFuzzyCompare(updated.confidence + 1.0, canonical->confidence + 1.0)
        || updated.mentionCount != canonical->mentionCount
        || updated.accessCount != canonical->accessCount
        || updated.payload != canonical->payload;

    if (changed) {
        *canonical = updated;
    }
    return changed;
}

void applyMetadataCompaction(MemoryStore* store,
                             const QString& mode,
                             bool dryRun,
                             int maxChanges,
                             QSet<QString>* skippedSensitiveIds,
                             MemoryOrganizeStats* stats) {
    if (!modeIncludes(mode, QStringLiteral("compact"))) return;

    const QList<MemoryEntry> entries = store->all();
    for (const MemoryEntry& entry : entries) {
        if (!hasChangeBudget(*stats, maxChanges)) return;
        if (isSensitive(entry, skippedSensitiveIds)) continue;

        MemoryEntry compacted = entry;
        if (!compactMetadata(&compacted)) continue;

        ++stats->metadataCompacted;
        countChange(stats);
        if (!dryRun) {
            store->updateEntryById(compacted);
        }
    }
}

void applyExpiry(MemoryStore* store,
                 const QString& mode,
                 bool dryRun,
                 int maxChanges,
                 const QDateTime& now,
                 QSet<QString>* skippedSensitiveIds,
                 MemoryOrganizeStats* stats) {
    if (!modeIncludes(mode, QStringLiteral("expire"))) return;

    const QList<MemoryEntry> entries = store->all();
    for (const MemoryEntry& entry : entries) {
        if (!hasChangeBudget(*stats, maxChanges)) return;
        if (entry.status != MemoryStatus::Active) continue;
        if (isSensitive(entry, skippedSensitiveIds)) continue;
        if (isProtectedFromExpiry(entry)) continue;
        if (!entry.expiresAt.isValid() || entry.expiresAt > now) continue;

        ++stats->expired;
        countChange(stats);
        if (!dryRun) {
            QJsonObject patch;
            patch[QStringLiteral("organized_at")] = dateTimeText(now);
            patch[QStringLiteral("organize_reason")] = QStringLiteral("expires_at_elapsed");
            store->updateStatusById(entry.id, MemoryStatus::Expired, patch);
        }
    }
}

void applyOperationalArchive(MemoryStore* store,
                             const QString& mode,
                             bool dryRun,
                             int maxChanges,
                             const QDateTime& now,
                             int shortTermDays,
                             int eventDays,
                             QSet<QString>* skippedSensitiveIds,
                             MemoryOrganizeStats* stats) {
    if (!modeIncludes(mode, QStringLiteral("archive"))) return;

    const QList<MemoryEntry> entries = store->all();
    for (const MemoryEntry& entry : entries) {
        if (!hasChangeBudget(*stats, maxChanges)) return;
        if (isSensitive(entry, skippedSensitiveIds)) continue;
        if (!shouldArchiveOperationalMemory(entry, now, shortTermDays, eventDays)) continue;

        ++stats->archived;
        countChange(stats);
        if (!dryRun) {
            QJsonObject patch;
            patch[QStringLiteral("organized_at")] = dateTimeText(now);
            patch[QStringLiteral("organize_reason")] = QStringLiteral("old_operational_memory");
            store->updateStatusById(entry.id, MemoryStatus::Archived, patch);
        }
    }
}

void applyDuplicateMerge(MemoryStore* store,
                         const QString& mode,
                         bool dryRun,
                         int maxChanges,
                         const QDateTime& now,
                         QSet<QString>* skippedSensitiveIds,
                         MemoryOrganizeStats* stats) {
    if (!modeIncludes(mode, QStringLiteral("merge_duplicates"))) return;

    QHash<QString, QList<MemoryEntry>> groups;
    for (const MemoryEntry& entry : store->all()) {
        if (entry.status != MemoryStatus::Active) continue;
        if (isSensitive(entry, skippedSensitiveIds)) continue;
        const QString key = duplicateKeyFor(entry);
        if (key.isEmpty()) continue;
        groups[key].append(entry);
    }

    for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
        if (!hasChangeBudget(*stats, maxChanges)) return;
        QList<MemoryEntry> group = it.value();
        if (group.size() < 2) continue;

        std::sort(group.begin(), group.end(), [](const MemoryEntry& a, const MemoryEntry& b) {
            return canonicalLessPreferred(b, a);
        });

        MemoryEntry canonical = group.first();
        bool canonicalChanged = false;
        for (int i = 1; i < group.size(); ++i) {
            canonicalChanged = mergeDuplicateIntoCanonical(&canonical, group.at(i)) || canonicalChanged;
        }

        if (canonicalChanged && hasChangeBudget(*stats, maxChanges)) {
            ++stats->canonicalUpdated;
            countChange(stats);
            if (!dryRun) {
                store->updateEntryById(canonical);
            }
        }

        for (int i = 1; i < group.size(); ++i) {
            if (!hasChangeBudget(*stats, maxChanges)) return;
            const MemoryEntry& duplicate = group.at(i);

            ++stats->superseded;
            countChange(stats);
            if (!dryRun) {
                if (!store->relationGraph().hasRelation(canonical.id, duplicate.id, MemoryRelationType::Supersedes)) {
                    MemoryRelation rel;
                    rel.fromMemoryId = canonical.id;
                    rel.toMemoryId = duplicate.id;
                    rel.type = MemoryRelationType::Supersedes;
                    rel.weight = 1.0;
                    rel.confidence = 1.0;
                    if (store->relationGraph().addRelation(rel)) {
                        ++stats->relationsCreated;
                    }
                }

                QJsonObject patch;
                patch[QStringLiteral("superseded_by")] = canonical.id;
                patch[QStringLiteral("organized_at")] = dateTimeText(now);
                patch[QStringLiteral("organize_reason")] = QStringLiteral("exact_duplicate");
                store->updateStatusById(duplicate.id, MemoryStatus::Superseded, patch);
            } else if (!store->relationGraph().hasRelation(canonical.id, duplicate.id, MemoryRelationType::Supersedes)) {
                ++stats->relationsCreated;
            }
        }
    }
}

} // namespace

MemoryOrganizeTool::MemoryOrganizeTool(MemoryStore* memoryStore)
    : AITool(
          QStringLiteral("memory_organize"),
          QStringLiteral("安全整理桌宠长期记忆。适合在 idle/proactive 空闲维护时调用；可过期旧短期记忆、归档旧工具事件、合并完全重复的记忆元数据并压缩标签/证据。不会物理删除用户记忆，结果只返回统计数量，不返回记忆正文。"),
          ToolCategory::Action)
    , m_memoryStore(memoryStore) {}

QJsonObject MemoryOrganizeTool::parameterSchema() const {
    QJsonObject schema;
    schema[QStringLiteral("type")] = QStringLiteral("object");

    QJsonObject properties;
    QJsonObject mode = makeStringProperty(
        QStringLiteral("整理模式：auto、report、expire、archive、merge_duplicates、compact。默认 auto。"),
        QStringLiteral("auto"));
    QJsonArray modeEnum;
    modeEnum.append(QStringLiteral("auto"));
    modeEnum.append(QStringLiteral("report"));
    modeEnum.append(QStringLiteral("expire"));
    modeEnum.append(QStringLiteral("archive"));
    modeEnum.append(QStringLiteral("merge_duplicates"));
    modeEnum.append(QStringLiteral("compact"));
    mode[QStringLiteral("enum")] = modeEnum;

    properties[QStringLiteral("mode")] = mode;
    properties[QStringLiteral("dry_run")] = makeIntegerProperty(
        QStringLiteral("是否只报告将要进行的整理而不写入，1=true，0=false。report 模式会强制 dry_run。"),
        0);
    properties[QStringLiteral("max_changes")] = makeIntegerProperty(
        QStringLiteral("本次最多应用多少条整理变更，默认 25，最高 100。"),
        kDefaultMaxChanges);
    properties[QStringLiteral("older_than_days")] = makeIntegerProperty(
        QStringLiteral("archive 模式下归档多少天以前的短期/工具事件记忆；未传时短期默认 2 天、工具事件默认 7 天。"),
        7);

    schema[QStringLiteral("properties")] = properties;
    return schema;
}

bool MemoryOrganizeTool::validate(const QJsonObject& params) const {
    const QString mode = params.value(QStringLiteral("mode")).toString(QStringLiteral("auto"));
    return isAllowedMode(mode);
}

ToolResult MemoryOrganizeTool::execute(const QJsonObject& params) {
    if (!m_memoryStore) {
        return ToolResult::fail(QStringLiteral("MemoryStore 未配置"));
    }

    const QDateTime now = QDateTime::currentDateTimeUtc();
    QString mode = params.value(QStringLiteral("mode")).toString(QStringLiteral("auto"));
    if (!isAllowedMode(mode)) {
        mode = QStringLiteral("auto");
    }

    bool dryRun = params.value(QStringLiteral("dry_run")).toInt(0) != 0;
    if (mode == QLatin1String("report")) {
        dryRun = true;
    }

    int maxChanges = params.value(QStringLiteral("max_changes")).toInt(kDefaultMaxChanges);
    maxChanges = qBound(1, maxChanges, kMaxChangesLimit);

    const bool customAge = params.contains(QStringLiteral("older_than_days"));
    const int archiveDays = qBound(1, params.value(QStringLiteral("older_than_days")).toInt(7), 365);
    const int shortTermArchiveDays = customAge ? archiveDays : 2;
    const int eventArchiveDays = customAge ? archiveDays : 7;

    MemoryOrganizeStats stats;
    stats.mode = mode;
    stats.dryRun = dryRun;
    stats.scanned = m_memoryStore->all().size();

    if (mode == QLatin1String("auto")
        && !dryRun
        && m_lastAppliedAt.isValid()
        && m_lastAppliedAt.secsTo(now) < kAutoCooldownSecs) {
        stats.skippedCooldown = true;
        return ToolResult::ok(stats.toJson());
    }

    QSet<QString> skippedSensitiveIds;

    applyMetadataCompaction(m_memoryStore,
                            mode,
                            dryRun,
                            maxChanges,
                            &skippedSensitiveIds,
                            &stats);
    applyExpiry(m_memoryStore,
                mode,
                dryRun,
                maxChanges,
                now,
                &skippedSensitiveIds,
                &stats);
    applyOperationalArchive(m_memoryStore,
                            mode,
                            dryRun,
                            maxChanges,
                            now,
                            shortTermArchiveDays,
                            eventArchiveDays,
                            &skippedSensitiveIds,
                            &stats);
    applyDuplicateMerge(m_memoryStore,
                        mode,
                        dryRun,
                        maxChanges,
                        now,
                        &skippedSensitiveIds,
                        &stats);

    stats.skippedSensitive = skippedSensitiveIds.size();

    if (!dryRun && stats.changed > 0) {
        QString error;
        stats.snapshotSaved = m_memoryStore->save(&error);
        stats.snapshotError = error;
    }

    if (mode == QLatin1String("auto") && !dryRun) {
        m_lastAppliedAt = now;
    }

    return ToolResult::ok(stats.toJson());
}
