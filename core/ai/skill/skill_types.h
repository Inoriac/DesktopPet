//
// Skill types — reusable, generalized workflow definitions
//

#ifndef DESKTOP_PET_SKILL_TYPES_H
#define DESKTOP_PET_SKILL_TYPES_H

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

struct SkillStep {
    QString instruction;
    QString toolHint;
    QString condition;

    QJsonObject toJson() const;
    static SkillStep fromJson(const QJsonObject& object);
};

struct SkillEntry {
    QString id;
    QString name;
    QString description;
    QString domain;
    QString abstractGoal;

    QStringList triggerPatterns;
    QStringList tags;
    QStringList requiredTools;
    QStringList preconditions;
    QStringList postconditions;

    QList<SkillStep> steps;
    QJsonObject parameterSchema;

    int useCount = 0;
    int successCount = 0;
    int failureCount = 0;
    int version = 1;

    QDateTime createdAt;
    QDateTime updatedAt;
    QDateTime lastUsedAt;

    double successRate() const;
    QJsonObject toJson() const;
    static SkillEntry fromJson(const QJsonObject& object);
};

#endif // DESKTOP_PET_SKILL_TYPES_H
