//
// SkillMatcher — generalized skill matching
//

#include "skill_matcher.h"
#include "skill_store.h"

#include <algorithm>
#include <cmath>

#include <QRegularExpression>
#include <QSet>

namespace {

QString joinedSkillText(const SkillEntry& entry) {
    QString text;
    text += entry.name + QStringLiteral("\n");
    text += entry.description + QStringLiteral("\n");
    text += entry.abstractGoal + QStringLiteral("\n");
    text += entry.domain + QStringLiteral("\n");
    text += entry.tags.join(QStringLiteral(" ")) + QStringLiteral("\n");
    for (const SkillStep& step : entry.steps) {
        text += step.instruction + QStringLiteral(" ");
    }
    return text.toLower();
}

QString formatSteps(const QList<SkillStep>& steps) {
    QStringList lines;
    for (int i = 0; i < steps.size(); ++i) {
        const SkillStep& step = steps[i];
        QString line = QStringLiteral("  %1. %2").arg(i + 1).arg(step.instruction);
        if (!step.toolHint.isEmpty()) {
            line += QStringLiteral(" [工具: %1]").arg(step.toolHint);
        }
        if (!step.condition.isEmpty()) {
            line += QStringLiteral(" (条件: %1)").arg(step.condition);
        }
        lines.append(line);
    }
    return lines.join(QStringLiteral("\n"));
}

}

QList<MatchedSkill> SkillMatcher::match(const SkillStore& store,
                                         const QString& query,
                                         int limit) const {
    QList<MatchedSkill> result;
    const QString queryLower = query.toLower().trimmed();
    const QStringList tokens = tokenize(query);

    if (tokens.isEmpty() && queryLower.isEmpty()) return result;

    for (const SkillEntry& entry : store.all()) {
        QStringList reasons;
        const double score = scoreSkill(entry, queryLower, tokens, &reasons);
        if (score < m_minScore) continue;

        MatchedSkill matched;
        matched.entry = entry;
        matched.score = score;
        matched.reasons = reasons;
        result.append(matched);
    }

    std::sort(result.begin(), result.end(), [](const MatchedSkill& a, const MatchedSkill& b) {
        return a.score > b.score;
    });

    while (result.size() > limit) {
        result.removeLast();
    }

    return result;
}

QStringList SkillMatcher::formatForContext(const QList<MatchedSkill>& skills) const {
    QStringList lines;
    for (int i = 0; i < skills.size(); ++i) {
        const SkillEntry& entry = skills[i].entry;

        QString header = QStringLiteral("=== 可参考技能 %1: %2 ===").arg(i + 1).arg(entry.name);
        lines.append(header);

        if (!entry.description.isEmpty()) {
            lines.append(QStringLiteral("描述: %1").arg(entry.description));
        }
        if (!entry.abstractGoal.isEmpty()) {
            lines.append(QStringLiteral("目标: %1").arg(entry.abstractGoal));
        }
        if (!entry.domain.isEmpty()) {
            lines.append(QStringLiteral("领域: %1").arg(entry.domain));
        }

        if (!entry.parameterSchema.isEmpty()) {
            const QStringList paramKeys = entry.parameterSchema.keys();
            if (!paramKeys.isEmpty()) {
                lines.append(QStringLiteral("参数: %1").arg(paramKeys.join(QStringLiteral(", "))));
            }
        }

        if (!entry.preconditions.isEmpty()) {
            lines.append(QStringLiteral("前置条件: %1").arg(entry.preconditions.join(QStringLiteral("; "))));
        }

        if (!entry.steps.isEmpty()) {
            lines.append(QStringLiteral("步骤:"));
            lines.append(formatSteps(entry.steps));
        }

        if (!entry.postconditions.isEmpty()) {
            lines.append(QStringLiteral("预期结果: %1").arg(entry.postconditions.join(QStringLiteral("; "))));
        }

        if (entry.useCount > 0) {
            lines.append(QStringLiteral("历史: 使用%1次, 成功率%2%")
                             .arg(entry.useCount)
                             .arg(qRound(entry.successRate() * 100)));
        }
    }
    return lines;
}

QStringList SkillMatcher::tokenize(const QString& text) const {
    QString normalized = text.toLower().trimmed();
    normalized.replace(QRegularExpression(QStringLiteral("[^a-z0-9_\\x{4e00}-\\x{9fa5}]+")), QStringLiteral(" "));

    QStringList tokens;
    const QStringList parts = normalized.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    QSet<QString> seen;
    for (const QString& part : parts) {
        if (part.size() < 2 || seen.contains(part)) continue;
        seen.insert(part);
        tokens.append(part);
    }
    return tokens;
}

double SkillMatcher::scoreSkill(const SkillEntry& entry,
                                 const QString& queryLower,
                                 const QStringList& tokens,
                                 QStringList* reasons) const {
    double score = 0.0;

    score += scoreTriggerPatterns(entry, queryLower, tokens, reasons);
    score += scoreSemanticFields(entry, tokens, reasons);
    score += scoreReliability(entry, reasons);

    return score;
}

double SkillMatcher::scoreTriggerPatterns(const SkillEntry& entry,
                                           const QString& queryLower,
                                           const QStringList& tokens,
                                           QStringList* reasons) const {
    double score = 0.0;

    for (const QString& pattern : entry.triggerPatterns) {
        const QString patternLower = pattern.toLower().trimmed();
        if (patternLower.isEmpty()) continue;

        if (queryLower.contains(patternLower)) {
            score += 4.0;
            if (reasons) reasons->append(QStringLiteral("trigger_exact"));
            continue;
        }

        const QStringList patternTokens = tokenize(patternLower);
        if (patternTokens.isEmpty()) continue;

        int matched = 0;
        for (const QString& pt : patternTokens) {
            for (const QString& qt : tokens) {
                if (qt.contains(pt) || pt.contains(qt)) {
                    ++matched;
                    break;
                }
            }
        }

        const double ratio = static_cast<double>(matched) / static_cast<double>(patternTokens.size());
        if (ratio >= 0.5) {
            score += ratio * 3.0;
            if (reasons && !reasons->contains(QStringLiteral("trigger_partial"))) {
                reasons->append(QStringLiteral("trigger_partial"));
            }
        }
    }

    return score;
}

double SkillMatcher::scoreSemanticFields(const SkillEntry& entry,
                                          const QStringList& tokens,
                                          QStringList* reasons) const {
    if (tokens.isEmpty()) return 0.0;

    const QString searchText = joinedSkillText(entry);

    int hitCount = 0;
    for (const QString& token : tokens) {
        if (searchText.contains(token)) ++hitCount;
    }

    if (hitCount == 0) return 0.0;

    const double ratio = static_cast<double>(hitCount) / static_cast<double>(tokens.size());
    const double score = ratio * 3.0;

    if (reasons) reasons->append(QStringLiteral("semantic"));

    return score;
}

double SkillMatcher::scoreReliability(const SkillEntry& entry,
                                       QStringList* reasons) const {
    double score = 0.0;

    if (entry.useCount > 0) {
        const double rate = entry.successRate();
        score += rate * 1.5;

        const double usageBonus = std::min(static_cast<double>(entry.useCount) / 20.0, 1.0) * 0.5;
        score += usageBonus;

        if (reasons) reasons->append(QStringLiteral("reliability"));
    }

    return score;
}
