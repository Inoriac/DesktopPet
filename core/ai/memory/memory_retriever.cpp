#include "memory_retriever.h"

#include <algorithm>
#include <cmath>

#include <QDateTime>
#include <QRegularExpression>
#include <QSet>

#include "memory_store.h"

namespace {
bool containsType(const QList<MemoryType>& types, MemoryType type) {
    return std::find(types.cbegin(), types.cend(), type) != types.cend();
}

bool hasAllTags(const QStringList& entryTags, const QStringList& requiredTags) {
    for (const QString& requiredTag : requiredTags) {
        if (!entryTags.contains(requiredTag, Qt::CaseInsensitive)) {
            return false;
        }
    }
    return true;
}

QString joinedSearchText(const MemoryEntry& entry) {
    QString text;
    text += entry.key + "\n";
    text += entry.summary + "\n";
    text += entry.content + "\n";
    text += entry.scope + "\n";
    text += entry.tags.join(" ");
    return text.toLower();
}

QString confidenceLabel(double confidence) {
    if (confidence >= 0.85) {
        return QStringLiteral("高置信度");
    }
    if (confidence >= 0.55) {
        return QStringLiteral("中置信度");
    }
    if (confidence > 0.0) {
        return QStringLiteral("低置信度");
    }
    return QStringLiteral("未知置信度");
}

QString bestSummary(const MemoryEntry& entry) {
    if (!entry.summary.trimmed().isEmpty()) {
        return entry.summary.trimmed();
    }
    if (!entry.content.trimmed().isEmpty()) {
        return entry.content.trimmed();
    }
    return entry.key.trimmed();
}
}

QList<RetrievedMemory> MemoryRetriever::retrieve(const MemoryStore& store,
                                                 const MemoryQuery& query) const {
    QList<RetrievedMemory> result;
    const QStringList tokens = tokenize(query.text);
    const int limit = query.limit <= 0 ? 8 : query.limit;

    for (const MemoryEntry& entry : store.all()) {
        if (!query.includeInactive && entry.status != MemoryStatus::Active) {
            continue;
        }
        if (!query.includeSensitive && entry.privacyLevel == PrivacyLevel::Sensitive) {
            continue;
        }
        if (!query.requiredTags.isEmpty() && !hasAllTags(entry.tags, query.requiredTags)) {
            continue;
        }

        QStringList reasons;
        const double score = scoreEntry(entry, query, tokens, &reasons);
        if (score <= 0.0) {
            continue;
        }

        RetrievedMemory memory;
        memory.entry = entry;
        memory.score = score;
        memory.reasons = reasons;
        result.append(memory);
    }

    std::sort(result.begin(), result.end(), [](const RetrievedMemory& left, const RetrievedMemory& right) {
        if (std::abs(left.score - right.score) > 0.0001) {
            return left.score > right.score;
        }
        return left.entry.updatedAt > right.entry.updatedAt;
    });

    while (result.size() > limit) {
        result.removeLast();
    }
    return result;
}

QStringList MemoryRetriever::formatForContext(const QList<RetrievedMemory>& memories) const {
    QStringList lines;
    int index = 1;
    for (const RetrievedMemory& memory : memories) {
        const MemoryEntry& entry = memory.entry;
        const QString summary = bestSummary(entry);
        if (summary.isEmpty()) {
            continue;
        }

        QStringList labels;
        labels.append(memoryTypeToString(entry.type));
        labels.append(confidenceLabel(entry.confidence));
        if (!entry.scope.trimmed().isEmpty()) {
            labels.append(entry.scope.trimmed());
        }

        lines.append(QStringLiteral("%1. [%2] %3")
                         .arg(index++)
                         .arg(labels.join(QStringLiteral("/")), summary));
    }
    return lines;
}

QStringList MemoryRetriever::tokenize(const QString& text) const {
    QString normalized = text.toLower().trimmed();
    normalized.replace(QRegularExpression(QStringLiteral("[^a-z0-9_\\u4e00-\\u9fa5]+")), QStringLiteral(" "));

    QStringList tokens;
    const QStringList parts = normalized.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    QSet<QString> seen;
    for (const QString& part : parts) {
        if (part.size() < 2 || seen.contains(part)) {
            continue;
        }
        seen.insert(part);
        tokens.append(part);
    }
    return tokens;
}

double MemoryRetriever::scoreEntry(const MemoryEntry& entry,
                                   const MemoryQuery& query,
                                   const QStringList& tokens,
                                   QStringList* reasons) const {
    double score = 0.0;
    const QString searchText = joinedSearchText(entry);

    double keywordScore = 0.0;
    for (const QString& token : tokens) {
        if (searchText.contains(token)) {
            keywordScore += 1.0;
        }
    }
    if (!tokens.isEmpty()) {
        keywordScore /= tokens.size();
        score += keywordScore * 4.0;
        if (keywordScore > 0.0 && reasons) {
            reasons->append(QStringLiteral("keyword"));
        }
    }

    if (!query.preferredTypes.isEmpty() && containsType(query.preferredTypes, entry.type)) {
        score += 2.0;
        if (reasons) {
            reasons->append(QStringLiteral("type"));
        }
    }

    score += std::clamp(entry.importance, 0.0, 1.0) * 1.6;
    score += std::clamp(entry.confidence, 0.0, 1.0) * 1.2;
    score += std::clamp(entry.strength, 0.0, 1.0) * 0.8;

    const QDateTime reference = entry.updatedAt.isValid() ? entry.updatedAt : entry.createdAt;
    if (reference.isValid()) {
        const qint64 ageDays = std::max<qint64>(0, reference.daysTo(QDateTime::currentDateTimeUtc()));
        const double recency = 1.0 / (1.0 + static_cast<double>(ageDays) / 30.0);
        score += recency * 0.8;
    }

    if (entry.privacyLevel == PrivacyLevel::Personal) {
        score -= 0.2;
    }
    if (entry.privacyLevel == PrivacyLevel::Sensitive) {
        score -= 4.0;
    }

    if (tokens.isEmpty() && query.preferredTypes.isEmpty() && query.requiredTags.isEmpty()) {
        score += 0.5;
    }

    return score;
}
