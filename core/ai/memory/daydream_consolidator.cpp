#include "daydream_consolidator.h"

#include <algorithm>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QHash>
#include <QSet>

#include "memory_store.h"
#include "partition_policy.h"

namespace {

QString normalizedJsonPayload(QString response) {
    response = response.trimmed();
    if (response.startsWith(QStringLiteral("```"))) {
        const int firstNewline = response.indexOf('\n');
        const int closingFence = response.lastIndexOf(QStringLiteral("```"));
        if (firstNewline >= 0 && closingFence > firstNewline) {
            response = response.mid(firstNewline + 1, closingFence - firstNewline - 1).trimmed();
        }
    }

    const int arrayStart = response.indexOf('[');
    const int arrayEnd = response.lastIndexOf(']');
    if (arrayStart >= 0 && arrayEnd >= arrayStart) {
        response = response.mid(arrayStart, arrayEnd - arrayStart + 1);
    }
    return response;
}

bool parseAction(const QString& value, DaydreamConsolidator::Action* action) {
    const QString normalized = value.trimmed().toLower();
    if (normalized == QLatin1String("create")) {
        *action = DaydreamConsolidator::Action::Create;
    } else if (normalized == QLatin1String("update")) {
        *action = DaydreamConsolidator::Action::Update;
    } else if (normalized == QLatin1String("keep_both")) {
        *action = DaydreamConsolidator::Action::KeepBoth;
    } else if (normalized == QLatin1String("discard")) {
        *action = DaydreamConsolidator::Action::Discard;
    } else if (normalized == QLatin1String("preserve")) {
        *action = DaydreamConsolidator::Action::Preserve;
    } else {
        return false;
    }
    return true;
}

bool parseTargetType(const QString& value, MemoryType* type) {
    const QString normalized = value.trimmed().toLower();
    if (normalized == QLatin1String("semantic")) {
        *type = MemoryType::Semantic;
    } else if (normalized == QLatin1String("episodic")) {
        *type = MemoryType::Episodic;
    } else if (normalized == QLatin1String("preference")) {
        *type = MemoryType::Preference;
    } else if (normalized == QLatin1String("procedural")) {
        *type = MemoryType::Procedural;
    } else {
        return false;
    }
    return true;
}

QStringList jsonStringList(const QJsonArray& values) {
    QStringList result;
    for (const QJsonValue& value : values) {
        const QString text = value.toString().trimmed().left(64);
        if (!text.isEmpty() && !result.contains(text, Qt::CaseInsensitive)) {
            result.append(text);
        }
    }
    return result;
}

bool sameRevision(const MemoryEntry& current, const MemoryEntry& snapshot) {
    return current.id == snapshot.id
        && current.partition == snapshot.partition
        && current.status == snapshot.status
        && current.updatedAt == snapshot.updatedAt
        && current.content == snapshot.content
        && current.summary == snapshot.summary
        && current.mentionCount == snapshot.mentionCount;
}

int relevanceScore(const MemoryEntry& candidate, const QList<MemoryEntry>& batch) {
    int score = 0;
    for (const MemoryEntry& source : batch) {
        for (const QString& tag : source.tags) {
            if (candidate.tags.contains(tag, Qt::CaseInsensitive)) score += 3;
        }
        const QString sourceText = source.summary.trimmed();
        if (!sourceText.isEmpty()
            && candidate.summary.contains(sourceText.left(24), Qt::CaseInsensitive)) {
            score += 2;
        }
    }
    return score;
}

} // namespace

DaydreamConsolidator::DaydreamConsolidator(MemoryStore& store)
    : m_store(store) {}

int DaydreamConsolidator::pendingCount() const {
    int count = 0;
    for (const MemoryEntry& entry : m_store.all()) {
        if (entry.partition == QLatin1String("hippocampus")
            && entry.status == MemoryStatus::Active) {
            ++count;
        }
    }
    return count;
}

DaydreamConsolidator::Snapshot DaydreamConsolidator::createSnapshot(int maxItems) const {
    Snapshot snapshot;
    if (maxItems <= 0) return snapshot;

    for (const MemoryEntry& entry : m_store.all()) {
        if (entry.partition == QLatin1String("hippocampus")
            && entry.status == MemoryStatus::Active) {
            snapshot.items.append(entry);
        }
    }
    std::sort(snapshot.items.begin(), snapshot.items.end(),
              [](const MemoryEntry& a, const MemoryEntry& b) {
                  if (a.createdAt == b.createdAt) return a.id < b.id;
                  return a.createdAt < b.createdAt;
              });
    if (snapshot.items.size() > maxItems) {
        snapshot.items = snapshot.items.mid(0, maxItems);
    }
    return snapshot;
}

QList<MemoryEntry> DaydreamConsolidator::relatedLongTermMemories(
    const QList<MemoryEntry>& batch, int limit) const {
    QList<MemoryEntry> candidates;
    for (const MemoryEntry& entry : m_store.all()) {
        if (entry.partition == QLatin1String("hippocampus")
            || entry.status != MemoryStatus::Active
            || entry.privacyLevel == PrivacyLevel::Sensitive) {
            continue;
        }
        candidates.append(entry);
    }
    std::sort(candidates.begin(), candidates.end(),
              [&batch](const MemoryEntry& a, const MemoryEntry& b) {
                  const int aScore = relevanceScore(a, batch);
                  const int bScore = relevanceScore(b, batch);
                  if (aScore != bScore) return aScore > bScore;
                  return a.updatedAt > b.updatedAt;
              });
    return candidates.mid(0, qMax(0, limit));
}

bool DaydreamConsolidator::parseDecisions(const QString& response,
                                          const QList<MemoryEntry>& batch,
                                          const QList<MemoryEntry>& allowedUpdateTargets,
                                          QList<Decision>* decisions,
                                          QString* errorMessage) {
    if (!decisions) return false;
    decisions->clear();

    QSet<QString> expectedIds;
    for (const MemoryEntry& entry : batch) expectedIds.insert(entry.id);
    QSet<QString> allowedTargetIds;
    for (const MemoryEntry& entry : allowedUpdateTargets) allowedTargetIds.insert(entry.id);

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        normalizedJsonPayload(response).toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        if (errorMessage) *errorMessage = QStringLiteral("Daydream 返回的内容不是有效 JSON 数组");
        return false;
    }

    QSet<QString> seenIds;
    for (const QJsonValue& value : document.array()) {
        if (!value.isObject()) {
            if (errorMessage) *errorMessage = QStringLiteral("Daydream 决策项必须是对象");
            decisions->clear();
            return false;
        }

        const QJsonObject object = value.toObject();
        Decision decision;
        decision.sourceId = object.value(QStringLiteral("source_id")).toString().trimmed();
        if (!expectedIds.contains(decision.sourceId) || seenIds.contains(decision.sourceId)) {
            if (errorMessage) *errorMessage = QStringLiteral("Daydream 决策 source_id 缺失、重复或不属于当前批次");
            decisions->clear();
            return false;
        }
        if (!parseAction(object.value(QStringLiteral("action")).toString(), &decision.action)) {
            if (errorMessage) *errorMessage = QStringLiteral("Daydream 决策 action 无效");
            decisions->clear();
            return false;
        }

        if (decision.action == Action::Create
            || decision.action == Action::Update
            || decision.action == Action::KeepBoth) {
            if (!parseTargetType(object.value(QStringLiteral("target_partition")).toString(),
                                 &decision.targetType)) {
                if (errorMessage) *errorMessage = QStringLiteral("Daydream 决策 target_partition 无效");
                decisions->clear();
                return false;
            }
        }

        decision.targetMemoryId = object.value(QStringLiteral("target_memory_id")).toString().trimmed();
        decision.mergedContent = object.value(QStringLiteral("merged_content")).toString().trimmed().left(2000);
        decision.qualityScore = qBound(0.0,
                                       object.value(QStringLiteral("quality_score")).toDouble(5.0),
                                       10.0);
        decision.tags = jsonStringList(object.value(QStringLiteral("new_tags")).toArray()).mid(0, 8);
        if (decision.action == Action::Update
            && !allowedTargetIds.contains(decision.targetMemoryId)) {
            if (errorMessage) *errorMessage = QStringLiteral("update 目标不在当前批次的历史记忆白名单中");
            decisions->clear();
            return false;
        }
        if (decision.action == Action::Update) {
            for (const MemoryEntry& target : allowedUpdateTargets) {
                if (target.id == decision.targetMemoryId) {
                    decision.expectedTarget = target;
                    break;
                }
            }
        }

        seenIds.insert(decision.sourceId);
        decisions->append(decision);
    }

    if (seenIds != expectedIds) {
        if (errorMessage) *errorMessage = QStringLiteral("Daydream 决策未覆盖当前批次的全部源记忆");
        decisions->clear();
        return false;
    }
    return true;
}

bool DaydreamConsolidator::requiresModelDecision(const MemoryEntry& entry) {
    return entry.source != QLatin1String("assistant_inferred")
        && entry.source != QLatin1String("assistant_response")
        && !entry.tags.contains(QStringLiteral("assistant"), Qt::CaseInsensitive);
}

QList<DaydreamConsolidator::Decision> DaydreamConsolidator::hardcodedDecisions(
    const QList<MemoryEntry>& batch) {
    QList<Decision> decisions;
    for (const MemoryEntry& entry : batch) {
        Decision decision;
        decision.sourceId = entry.id;
        if (!requiresModelDecision(entry)) {
            decision.action = Action::Discard;
        } else if (entry.mentionCount >= 2 || entry.emotionIntensity >= 0.7) {
            decision.action = Action::Create;
            decision.targetType = MemoryType::Episodic;
            decision.qualityScore = qBound(0.0, entry.importance * 10.0 + 1.0, 10.0);
            decision.tags = entry.tags;
        } else {
            decision.action = Action::Discard;
        }
        decisions.append(decision);
    }
    return decisions;
}

bool DaydreamConsolidator::snapshotStillCurrent(const Snapshot& snapshot) const {
    for (const MemoryEntry& original : snapshot.items) {
        const MemoryEntry* current = m_store.findById(original.id);
        if (!current || !sameRevision(*current, original)) return false;
    }
    return true;
}

bool DaydreamConsolidator::updateTargetsStillCurrent(const QList<Decision>& decisions) const {
    for (const Decision& decision : decisions) {
        if (decision.action != Action::Update) continue;
        const MemoryEntry* current = m_store.findById(decision.targetMemoryId);
        if (decision.expectedTarget.id != decision.targetMemoryId
            || !current
            || !sameRevision(*current, decision.expectedTarget)) {
            return false;
        }
    }
    return true;
}

MemoryEntry DaydreamConsolidator::makeLongTermEntry(const MemoryEntry& source,
                                                    const Decision& decision) const {
    MemoryEntry consolidated;
    consolidated.type = decision.targetType;
    consolidated.status = MemoryStatus::Active;
    consolidated.privacyLevel = source.privacyLevel == PrivacyLevel::Sensitive
        ? PrivacyLevel::Sensitive
        : PrivacyLevel::Personal;
    consolidated.key = source.key;
    consolidated.summary = decision.mergedContent.isEmpty()
        ? source.summary
        : decision.mergedContent.left(160);
    consolidated.content = decision.mergedContent.isEmpty()
        ? source.content
        : decision.mergedContent;
    const auto appendTag = [&consolidated](const QString& rawTag) {
        const QString tag = rawTag.simplified().left(64);
        if (tag.isEmpty() || tag.compare(QStringLiteral("daydream_inbox"), Qt::CaseInsensitive) == 0) {
            return;
        }
        if (!consolidated.tags.contains(tag, Qt::CaseInsensitive)) consolidated.tags.append(tag);
    };
    for (const QString& tag : source.tags) appendTag(tag);
    for (const QString& tag : decision.tags) appendTag(tag);
    consolidated.scope = source.scope;
    consolidated.source = QStringLiteral("daydream");
    consolidated.importance = qBound(0.1, decision.qualityScore / 10.0, 1.0);
    consolidated.strength = consolidated.importance;
    consolidated.confidence = decision.action == Action::Update ? 0.8 : 0.75;
    consolidated.emotion = source.emotion;
    consolidated.emotionIntensity = source.emotionIntensity;
    consolidated.emotionConfidence = source.emotionConfidence;
    consolidated.mentionCount = source.mentionCount;
    consolidated.sourceMemoryIds = QStringList{source.id};
    consolidated.evidence = source.evidence;
    return consolidated;
}

bool DaydreamConsolidator::applyOne(const MemoryEntry& source,
                                    const Decision& decision,
                                    Stats* stats) {
    switch (decision.action) {
    case Action::Preserve:
        ++stats->preserved;
        return true;
    case Action::Discard:
        if (!m_store.removeEntryById(source.id)) return false;
        ++stats->discarded;
        return true;
    case Action::Create:
    case Action::KeepBoth: {
        const MemoryEntry created = m_store.addEntry(makeLongTermEntry(source, decision));
        if (created.id.isEmpty()
            || !m_store.tagCooccurrenceGraph().recordTags(created.tags)
            || !m_store.removeEntryById(source.id)) {
            return false;
        }
        ++stats->upgraded;
        return true;
    }
    case Action::Update: {
        MemoryEntry* target = m_store.findById(decision.targetMemoryId);
        if (!target || target->partition == QLatin1String("hippocampus")
            || target->status != MemoryStatus::Active) {
            return false;
        }
        MemoryEntry updated = *target;
        const MemoryEntry staged = makeLongTermEntry(source, decision);
        updated.type = staged.type;
        updated.partition = partitionToString(partitionForType(staged.type));
        updated.summary = staged.summary;
        updated.content = staged.content;
        updated.importance = qMax(updated.importance, staged.importance);
        updated.strength = qMax(updated.strength, staged.strength);
        updated.confidence = qMax(updated.confidence, staged.confidence);
        updated.updatedAt = QDateTime::currentDateTimeUtc();
        for (const QString& tag : staged.tags) {
            if (!updated.tags.contains(tag, Qt::CaseInsensitive)) updated.tags.append(tag);
        }
        if (!updated.sourceMemoryIds.contains(source.id)) updated.sourceMemoryIds.append(source.id);
        if (!m_store.updateEntryById(updated)
            || !m_store.tagCooccurrenceGraph().recordTags(updated.tags)
            || !m_store.removeEntryById(source.id)) {
            return false;
        }
        ++stats->updated;
        return true;
    }
    }
    return false;
}

DaydreamConsolidator::Stats DaydreamConsolidator::applyDecisions(
    const Snapshot& snapshot, const QList<Decision>& decisions) {
    Stats stats;
    stats.scanned = snapshot.size();
    if (snapshot.isEmpty()) {
        return stats;
    }
    if (decisions.size() != snapshot.size()) {
        stats.failed = snapshot.size();
        return stats;
    }

    QHash<QString, Decision> decisionsById;
    QSet<QString> updatedTargetIds;
    for (const Decision& decision : decisions) {
        if (decision.sourceId.isEmpty() || decisionsById.contains(decision.sourceId)) {
            stats.failed = snapshot.size();
            return stats;
        }
        if (decision.action == Action::Update
            && (decision.targetMemoryId.isEmpty()
                || updatedTargetIds.contains(decision.targetMemoryId))) {
            stats.failed = snapshot.size();
            return stats;
        }
        if (decision.action == Action::Update) updatedTargetIds.insert(decision.targetMemoryId);
        decisionsById.insert(decision.sourceId, decision);
    }
    for (const MemoryEntry& source : snapshot.items) {
        if (!decisionsById.contains(source.id)) {
            stats.failed = snapshot.size();
            return stats;
        }
    }

    if (!snapshotStillCurrent(snapshot) || !updateTargetsStillCurrent(decisions)) {
        stats.staleSnapshot = true;
        return stats;
    }
    if (!m_store.beginTransaction()) {
        stats.failed = snapshot.size();
        return stats;
    }

    bool ok = true;
    for (const MemoryEntry& source : snapshot.items) {
        if (!applyOne(source, decisionsById.value(source.id), &stats)) {
            ++stats.failed;
            ok = false;
            break;
        }
    }

    if (ok && m_store.commitTransaction()) {
        stats.committed = true;
    } else {
        m_store.rollbackTransaction();
    }
    m_store.load();
    return stats;
}

DaydreamConsolidator::Stats DaydreamConsolidator::runHardcodedDrain(int maxItems) {
    const Snapshot snapshot = createSnapshot(maxItems);
    if (snapshot.isEmpty()) return {};
    return applyDecisions(snapshot, hardcodedDecisions(snapshot.items));
}
