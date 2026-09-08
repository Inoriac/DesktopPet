#include "daydream_consolidator.h"

#include <algorithm>
#include <utility>

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QHash>
#include <QSet>

#include "batch_selector.h"
#include "hybrid_graph_builder.h"
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

QJsonObject memoryEntryToJson(const MemoryEntry& entry,
                              bool legacySecondPrecision) {
    QJsonObject object = entry.toJson();
    if (!legacySecondPrecision) return object;

    const auto setTimestamp = [&object](const QString& key,
                                        const QDateTime& timestamp) {
        object.insert(key, timestamp.isValid()
                               ? timestamp.toString(Qt::ISODate)
                               : QString());
    };
    setTimestamp(QStringLiteral("created_at"), entry.createdAt);
    setTimestamp(QStringLiteral("updated_at"), entry.updatedAt);
    setTimestamp(QStringLiteral("last_accessed_at"), entry.lastAccessedAt);
    setTimestamp(QStringLiteral("expires_at"), entry.expiresAt);
    return object;
}

QJsonObject decisionToJson(const DaydreamDecision& decision,
                           bool legacySecondPrecision) {
    QJsonObject object;
    object.insert(QStringLiteral("sourceId"), decision.sourceId);
    object.insert(QStringLiteral("action"), static_cast<int>(decision.action));
    object.insert(QStringLiteral("targetType"), static_cast<int>(decision.targetType));
    object.insert(QStringLiteral("targetMemoryId"), decision.targetMemoryId);
    object.insert(QStringLiteral("mergedContent"), decision.mergedContent);
    object.insert(QStringLiteral("qualityScore"), decision.qualityScore);
    object.insert(QStringLiteral("tags"), QJsonArray::fromStringList(decision.tags));
    if (!decision.expectedTarget.id.isEmpty()) {
        object.insert(QStringLiteral("expectedTarget"), memoryEntryToJson(
            decision.expectedTarget, legacySecondPrecision));
    }
    return object;
}

QJsonObject changeSetContent(const DaydreamChangeSet& changeSet,
                             bool legacySecondPrecision = false) {
    QJsonArray snapshot;
    for (const MemoryEntry& entry : changeSet.snapshot.items) {
        snapshot.append(memoryEntryToJson(entry, legacySecondPrecision));
    }
    QJsonArray decisions;
    for (const DaydreamDecision& decision : changeSet.decisions) {
        decisions.append(decisionToJson(decision, legacySecondPrecision));
    }
    return {
        {QStringLiteral("snapshot"), snapshot},
        {QStringLiteral("decisions"), decisions}
    };
}

QString jsonHash(const QJsonObject& object) {
    return QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(object).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex());
}

bool structurallyValid(const DaydreamChangeSet& changeSet) {
    if (changeSet.snapshot.size() != changeSet.decisions.size()) return false;
    QSet<QString> sources;
    for (const MemoryEntry& source : changeSet.snapshot.items) {
        if (source.id.trimmed().isEmpty() || sources.contains(source.id)) return false;
        sources.insert(source.id);
    }
    QSet<QString> decisions;
    QSet<QString> targets;
    for (const DaydreamDecision& decision : changeSet.decisions) {
        const int action = static_cast<int>(decision.action);
        const int targetType = static_cast<int>(decision.targetType);
        if (!sources.contains(decision.sourceId)
            || decisions.contains(decision.sourceId)
            || action < static_cast<int>(DaydreamAction::Preserve)
            || action > static_cast<int>(DaydreamAction::Discard)
            || targetType < static_cast<int>(MemoryType::Working)
            || targetType > static_cast<int>(MemoryType::Event)) {
            return false;
        }
        decisions.insert(decision.sourceId);
        if (decision.action == DaydreamAction::Update) {
            if (decision.targetMemoryId.isEmpty()
                || targets.contains(decision.targetMemoryId)
                || decision.expectedTarget.id != decision.targetMemoryId) {
                return false;
            }
            targets.insert(decision.targetMemoryId);
        }
    }
    return sources == decisions;
}

} // namespace

QJsonObject DaydreamChangeSet::toJson() const {
    QJsonObject object = changeSetContent(*this);
    object.insert(QStringLiteral("changeSetId"), changeSetId);
    return object;
}

QString DaydreamChangeSet::payloadHash() const {
    return jsonHash(toJson());
}

Result<DaydreamChangeSet, DomainError> DaydreamChangeSet::fromJson(
    const QJsonObject& object) {
    DaydreamChangeSet changeSet;
    changeSet.changeSetId = object.value(QStringLiteral("changeSetId"))
                                .toString().trimmed();
    const QJsonArray snapshot = object.value(QStringLiteral("snapshot")).toArray();
    for (const QJsonValue& value : snapshot) {
        if (!value.isObject()) {
            return Result<DaydreamChangeSet, DomainError>::failure(
                domainError(QStringLiteral("MEMORY_STORE_UNAVAILABLE"),
                            QStringLiteral("staged Daydream snapshot is invalid")));
        }
        changeSet.snapshot.items.append(MemoryEntry::fromJson(value.toObject()));
    }
    const QJsonArray decisions = object.value(QStringLiteral("decisions")).toArray();
    for (const QJsonValue& value : decisions) {
        if (!value.isObject()) {
            return Result<DaydreamChangeSet, DomainError>::failure(
                domainError(QStringLiteral("MEMORY_STORE_UNAVAILABLE"),
                            QStringLiteral("staged Daydream decision is invalid")));
        }
        const QJsonObject decisionObject = value.toObject();
        const int action = decisionObject.value(QStringLiteral("action")).toInt(-1);
        const int targetType = decisionObject.value(QStringLiteral("targetType")).toInt(-1);
        if (action < static_cast<int>(DaydreamAction::Preserve)
            || action > static_cast<int>(DaydreamAction::Discard)
            || targetType < static_cast<int>(MemoryType::Working)
            || targetType > static_cast<int>(MemoryType::Event)) {
            return Result<DaydreamChangeSet, DomainError>::failure(
                domainError(QStringLiteral("MEMORY_STORE_UNAVAILABLE"),
                            QStringLiteral("staged Daydream enum is invalid")));
        }
        DaydreamDecision decision;
        decision.sourceId = decisionObject.value(QStringLiteral("sourceId")).toString();
        decision.action = static_cast<DaydreamAction>(action);
        decision.targetType = static_cast<MemoryType>(targetType);
        decision.targetMemoryId = decisionObject
                                      .value(QStringLiteral("targetMemoryId")).toString();
        decision.mergedContent = decisionObject
                                     .value(QStringLiteral("mergedContent")).toString();
        decision.qualityScore = decisionObject
                                    .value(QStringLiteral("qualityScore")).toDouble();
        decision.tags = jsonStringList(
            decisionObject.value(QStringLiteral("tags")).toArray());
        if (decisionObject.value(QStringLiteral("expectedTarget")).isObject()) {
            decision.expectedTarget = MemoryEntry::fromJson(
                decisionObject.value(QStringLiteral("expectedTarget")).toObject());
        }
        changeSet.decisions.append(std::move(decision));
    }
    const QString currentHash = jsonHash(changeSetContent(changeSet));
    const QString legacyHash = jsonHash(changeSetContent(changeSet, true));
    if (changeSet.changeSetId.isEmpty()
        || (changeSet.changeSetId != currentHash
            && changeSet.changeSetId != legacyHash)) {
        return Result<DaydreamChangeSet, DomainError>::failure(
            domainError(QStringLiteral("MEMORY_STORE_UNAVAILABLE"),
                        QStringLiteral("staged Daydream change set hash is invalid")));
    }
    return Result<DaydreamChangeSet, DomainError>::success(std::move(changeSet));
}

DaydreamConsolidator::DaydreamConsolidator(MemoryStore& store)
    : m_store(store),
      m_batchSelector(std::make_unique<BatchSelector>(store)) {}

DaydreamConsolidator::~DaydreamConsolidator() = default;

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
    
    // Phase 4: 使用批次选择器（优先级评分 + 老化保底 + 锚点 + 情景簇扩展）
    QList<MemoryEntry> anchors;
    snapshot.items = m_batchSelector->selectBatch(maxItems, &anchors);
    
    // TODO: 记录 anchors 到 changeSet metadata 供审查验收
    
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

namespace {

// 兜底分区分类（pre-phase4-roadmap §一）：LLM 不可用时的启发式替代，
// 准确率低于模型但保证积压可被消化。关键词命中优先级：
// Preference > Procedural > Semantic > Episodic（默认）。
MemoryType heuristicTargetType(const MemoryEntry& entry) {
    const QString text = (entry.key + QLatin1Char(' ') + entry.summary
                          + QLatin1Char(' ') + entry.content).toLower();
    const bool hasPreferenceTag =
        entry.tags.contains(QStringLiteral("preference"), Qt::CaseInsensitive)
        || entry.tags.contains(QStringLiteral("偏好"), Qt::CaseInsensitive);
    if (hasPreferenceTag
        || text.contains(QStringLiteral("喜欢"))
        || text.contains(QStringLiteral("讨厌"))
        || text.contains(QStringLiteral("偏好"))
        || text.contains(QStringLiteral("习惯"))
        || text.contains(QLatin1String("likes"))
        || text.contains(QLatin1String("dislikes"))) {
        return MemoryType::Preference;
    }
    const bool hasTaskTag =
        entry.tags.contains(QStringLiteral("task"), Qt::CaseInsensitive)
        || entry.tags.contains(QStringLiteral("procedure"), Qt::CaseInsensitive);
    if (hasTaskTag
        || entry.source.contains(QLatin1String("tool"))
        || text.contains(QStringLiteral("步骤"))
        || text.contains(QStringLiteral("如何"))
        || text.contains(QStringLiteral("方法"))) {
        return MemoryType::Procedural;
    }
    if (entry.mentionCount >= 3
        || text.contains(QStringLiteral("叫做"))
        || text.contains(QStringLiteral("定义"))
        || text.contains(QStringLiteral("生日"))
        || text.contains(QStringLiteral("住在"))) {
        return MemoryType::Semantic;
    }
    return MemoryType::Episodic;
}

// 兜底质量评分：importance 主导，mention/emotion 辅助，clamp 0-10。
double heuristicQualityScore(const MemoryEntry& entry) {
    double score = entry.importance * 10.0;
    score += qMin(2.0, entry.mentionCount * 0.5);
    score += entry.emotionIntensity * 3.0;
    return qBound(0.0, score, 10.0);
}

}

QList<DaydreamConsolidator::Decision> DaydreamConsolidator::hardcodedDecisions(
    const QList<MemoryEntry>& batch) {
    QList<Decision> decisions;
    QSet<QString> seenContent;  // 批内去重：相同归一化正文只升级第一条
    for (const MemoryEntry& entry : batch) {
        Decision decision;
        decision.sourceId = entry.id;
        const QString normalized =
            (entry.summary + entry.content).simplified().toLower();
        if (!requiresModelDecision(entry)) {
            decision.action = Action::Discard;
        } else if (!normalized.isEmpty() && seenContent.contains(normalized)) {
            decision.action = Action::Discard;  // 批内重复
        } else if (entry.mentionCount >= 2
                   || entry.emotionIntensity >= 0.7
                   || entry.importance >= 0.6) {
            decision.action = Action::Create;
            decision.targetType = heuristicTargetType(entry);
            decision.qualityScore = heuristicQualityScore(entry);
            decision.tags = entry.tags;
            seenContent.insert(normalized);
        } else {
            decision.action = Action::Discard;
        }
        decisions.append(decision);
    }
    return decisions;
}

Result<DaydreamChangeSet, DomainError> DaydreamConsolidator::buildChangeSet(
    const Snapshot& snapshot,
    const QList<Decision>& decisions) const {
    if (decisions.size() != snapshot.size()) {
        return Result<DaydreamChangeSet, DomainError>::failure(
            domainError(QStringLiteral("MODEL_OUTPUT_INVALID"),
                        QStringLiteral("Daydream decisions do not cover the snapshot")));
    }
    QSet<QString> sourceIds;
    QSet<QString> updateTargets;
    for (const MemoryEntry& source : snapshot.items) {
        if (source.id.trimmed().isEmpty() || sourceIds.contains(source.id)) {
            return Result<DaydreamChangeSet, DomainError>::failure(
                domainError(QStringLiteral("MODEL_OUTPUT_INVALID"),
                            QStringLiteral("Daydream snapshot contains invalid sources")));
        }
        sourceIds.insert(source.id);
    }
    QSet<QString> decisionSources;
    for (const Decision& decision : decisions) {
        if (!sourceIds.contains(decision.sourceId)
            || decisionSources.contains(decision.sourceId)) {
            return Result<DaydreamChangeSet, DomainError>::failure(
                domainError(QStringLiteral("MODEL_OUTPUT_INVALID"),
                            QStringLiteral("Daydream decision sources are invalid")));
        }
        decisionSources.insert(decision.sourceId);
        if (decision.action == Action::Update) {
            if (decision.targetMemoryId.isEmpty()
                || updateTargets.contains(decision.targetMemoryId)
                || decision.expectedTarget.id != decision.targetMemoryId) {
                return Result<DaydreamChangeSet, DomainError>::failure(
                    domainError(QStringLiteral("MODEL_OUTPUT_INVALID"),
                                QStringLiteral("Daydream update target is invalid")));
            }
            updateTargets.insert(decision.targetMemoryId);
        }
    }
    if (decisionSources != sourceIds) {
        return Result<DaydreamChangeSet, DomainError>::failure(
            domainError(QStringLiteral("MODEL_OUTPUT_INVALID"),
                        QStringLiteral("Daydream decisions are incomplete")));
    }
    if (!snapshotStillCurrent(snapshot) || !updateTargetsStillCurrent(decisions)) {
        return Result<DaydreamChangeSet, DomainError>::failure(
            domainError(QStringLiteral("STATE_VERSION_CONFLICT"),
                        QStringLiteral("Daydream snapshot is stale")));
    }

    DaydreamChangeSet changeSet;
    changeSet.snapshot = snapshot;
    changeSet.decisions = decisions;
    changeSet.changeSetId = jsonHash(changeSetContent(changeSet));
    return Result<DaydreamChangeSet, DomainError>::success(std::move(changeSet));
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
                                    Stats* stats,
                                    MemoryEntry* resultingEntry) {
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
        if (resultingEntry) *resultingEntry = created;
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
        if (resultingEntry) *resultingEntry = updated;
        ++stats->updated;
        return true;
    }
    }
    return false;
}

DaydreamConsolidator::Stats DaydreamConsolidator::applyDecisions(
    const Snapshot& snapshot, const QList<Decision>& decisions) {
    const auto changeSet = buildChangeSet(snapshot, decisions);
    if (!changeSet.isOk()) {
        Stats stats;
        stats.scanned = snapshot.size();
        stats.failed = snapshot.size();
        stats.staleSnapshot = changeSet.error().code == QLatin1String("STATE_VERSION_CONFLICT");
        return stats;
    }
    return applyChangeSet(changeSet.value());
}

DaydreamConsolidator::Stats DaydreamConsolidator::applyChangeSet(
    const DaydreamChangeSet& changeSet) {
    Stats stats;
    stats.scanned = changeSet.snapshot.size();
    const QString expectedId = jsonHash(changeSetContent(changeSet));
    const QString legacyExpectedId = jsonHash(changeSetContent(changeSet, true));
    const QString payloadHash = changeSet.payloadHash();
    if (changeSet.changeSetId.isEmpty()
        || (changeSet.changeSetId != expectedId
            && changeSet.changeSetId != legacyExpectedId)
        || !structurallyValid(changeSet)) {
        stats.failed = changeSet.snapshot.size();
        return stats;
    }
    if (m_store.isSleepChangeFinalized(changeSet.changeSetId, payloadHash)) {
        stats.committed = true;
        return stats;
    }
    const auto validation = buildChangeSet(changeSet.snapshot, changeSet.decisions);
    if (!validation.isOk()) {
        stats.failed = changeSet.snapshot.size();
        stats.staleSnapshot = validation.error().code
            == QLatin1String("STATE_VERSION_CONFLICT");
        return stats;
    }
    if (!m_store.hasSleepChange(changeSet.changeSetId, payloadHash)) {
        StagedMemoryChange legacy;
        legacy.sessionId = QStringLiteral("legacy:%1").arg(changeSet.changeSetId);
        legacy.changeId = changeSet.changeSetId;
        legacy.targetType = QStringLiteral("daydream_change_set");
        legacy.operation = QStringLiteral("apply");
        legacy.payload = changeSet.toJson();
        if (!m_store.stageSleepChange(legacy)) {
            stats.failed = changeSet.snapshot.size();
            return stats;
        }
    }
    QHash<QString, Decision> decisionsById;
    for (const Decision& decision : changeSet.decisions) {
        decisionsById.insert(decision.sourceId, decision);
    }
    if (!m_store.beginTransaction()) {
        stats.failed = changeSet.snapshot.size();
        return stats;
    }

    bool ok = true;
    QList<MemoryEntry> consolidatedResults;  // Phase 4.2：巩固产出，供混合建图
    for (const MemoryEntry& source : changeSet.snapshot.items) {
        MemoryEntry resulting;
        if (!applyOne(source, decisionsById.value(source.id), &stats, &resulting)) {
            ++stats.failed;
            ok = false;
            break;
        }
        if (!resulting.id.isEmpty()) {
            consolidatedResults.append(resulting);
        }
    }

    // Phase 4.2（设计 §10）：巩固路径同时维护记忆关系图与标签共现图。
    // 标签共现已在 applyOne 内记录；此处补关系图：批次内共现建图 + 边维护。
    // 均在同一 SQLite 事务内，失败随 ROLLBACK 原子撤销。
    if (ok) {
        HybridGraphBuilder graphBuilder(m_store.relationGraph());
        graphBuilder.buildForConsolidationBatch(consolidatedResults);

        // 边维护（悬空清理兼顾崩溃一致性；节点上限仅检查本批涉及节点，控制开销）
        QSet<QString> validMemoryIds;
        QStringList touchedNodeIds;
        const QList<MemoryEntry> allEntries = m_store.all();
        validMemoryIds.reserve(allEntries.size());
        for (const MemoryEntry& entry : allEntries) {
            validMemoryIds.insert(entry.id);
        }
        for (const MemoryEntry& entry : consolidatedResults) {
            touchedNodeIds.append(entry.id);
        }
        graphBuilder.maintain(validMemoryIds, touchedNodeIds);
    }

    if (ok && !m_store.finalizeSleepChange(
            changeSet.changeSetId, payloadHash)) {
        ++stats.failed;
        ok = false;
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
