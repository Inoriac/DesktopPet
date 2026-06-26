//
// SkillMatcher — matches queries against available skills with generalization
//

#ifndef DESKTOP_PET_SKILL_MATCHER_H
#define DESKTOP_PET_SKILL_MATCHER_H

#include <QList>
#include <QString>
#include <QStringList>

#include "skill_types.h"

class SkillStore;

struct MatchedSkill {
    SkillEntry entry;
    double score = 0.0;
    QStringList reasons;
};

class SkillMatcher {
public:
    void setMinScore(double minScore) { m_minScore = minScore; }

    QList<MatchedSkill> match(const SkillStore& store,
                              const QString& query,
                              int limit = 3) const;

    QStringList formatForContext(const QList<MatchedSkill>& skills) const;

private:
    QStringList tokenize(const QString& text) const;

    double scoreSkill(const SkillEntry& entry,
                      const QString& queryLower,
                      const QStringList& tokens,
                      QStringList* reasons) const;

    double scoreTriggerPatterns(const SkillEntry& entry,
                               const QString& queryLower,
                               const QStringList& tokens,
                               QStringList* reasons) const;

    double scoreSemanticFields(const SkillEntry& entry,
                               const QStringList& tokens,
                               QStringList* reasons) const;

    double scoreReliability(const SkillEntry& entry,
                            QStringList* reasons) const;

    double m_minScore = 1.5;
};

#endif // DESKTOP_PET_SKILL_MATCHER_H
