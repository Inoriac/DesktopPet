//
// Skill types — serialization
//

#include "skill_types.h"

#include <QJsonArray>

QJsonObject SkillStep::toJson() const {
    QJsonObject obj;
    obj[QStringLiteral("instruction")] = instruction;
    if (!toolHint.isEmpty()) {
        obj[QStringLiteral("tool_hint")] = toolHint;
    }
    if (!condition.isEmpty()) {
        obj[QStringLiteral("condition")] = condition;
    }
    return obj;
}

SkillStep SkillStep::fromJson(const QJsonObject& object) {
    SkillStep step;
    step.instruction = object.value(QStringLiteral("instruction")).toString();
    step.toolHint = object.value(QStringLiteral("tool_hint")).toString();
    step.condition = object.value(QStringLiteral("condition")).toString();
    return step;
}

double SkillEntry::successRate() const {
    const int total = successCount + failureCount;
    if (total <= 0) return 0.0;
    return static_cast<double>(successCount) / static_cast<double>(total);
}

QJsonObject SkillEntry::toJson() const {
    QJsonObject obj;
    obj[QStringLiteral("id")] = id;
    obj[QStringLiteral("name")] = name;
    obj[QStringLiteral("description")] = description;
    obj[QStringLiteral("domain")] = domain;
    obj[QStringLiteral("abstract_goal")] = abstractGoal;

    obj[QStringLiteral("trigger_patterns")] = QJsonArray::fromStringList(triggerPatterns);
    obj[QStringLiteral("tags")] = QJsonArray::fromStringList(tags);
    obj[QStringLiteral("required_tools")] = QJsonArray::fromStringList(requiredTools);
    obj[QStringLiteral("preconditions")] = QJsonArray::fromStringList(preconditions);
    obj[QStringLiteral("postconditions")] = QJsonArray::fromStringList(postconditions);

    QJsonArray stepsArray;
    for (const SkillStep& step : steps) {
        stepsArray.append(step.toJson());
    }
    obj[QStringLiteral("steps")] = stepsArray;

    if (!parameterSchema.isEmpty()) {
        obj[QStringLiteral("parameter_schema")] = parameterSchema;
    }

    obj[QStringLiteral("use_count")] = useCount;
    obj[QStringLiteral("success_count")] = successCount;
    obj[QStringLiteral("failure_count")] = failureCount;
    obj[QStringLiteral("version")] = version;

    if (createdAt.isValid()) {
        obj[QStringLiteral("created_at")] = createdAt.toUTC().toString(Qt::ISODate);
    }
    if (updatedAt.isValid()) {
        obj[QStringLiteral("updated_at")] = updatedAt.toUTC().toString(Qt::ISODate);
    }
    if (lastUsedAt.isValid()) {
        obj[QStringLiteral("last_used_at")] = lastUsedAt.toUTC().toString(Qt::ISODate);
    }

    return obj;
}

SkillEntry SkillEntry::fromJson(const QJsonObject& object) {
    SkillEntry entry;
    entry.id = object.value(QStringLiteral("id")).toString();
    entry.name = object.value(QStringLiteral("name")).toString();
    entry.description = object.value(QStringLiteral("description")).toString();
    entry.domain = object.value(QStringLiteral("domain")).toString();
    entry.abstractGoal = object.value(QStringLiteral("abstract_goal")).toString();

    const auto toStringList = [](const QJsonArray& array) {
        QStringList result;
        for (const QJsonValue& value : array) {
            const QString str = value.toString().trimmed();
            if (!str.isEmpty()) result.append(str);
        }
        return result;
    };

    entry.triggerPatterns = toStringList(object.value(QStringLiteral("trigger_patterns")).toArray());
    entry.tags = toStringList(object.value(QStringLiteral("tags")).toArray());
    entry.requiredTools = toStringList(object.value(QStringLiteral("required_tools")).toArray());
    entry.preconditions = toStringList(object.value(QStringLiteral("preconditions")).toArray());
    entry.postconditions = toStringList(object.value(QStringLiteral("postconditions")).toArray());

    const QJsonArray stepsArray = object.value(QStringLiteral("steps")).toArray();
    for (const QJsonValue& value : stepsArray) {
        entry.steps.append(SkillStep::fromJson(value.toObject()));
    }

    entry.parameterSchema = object.value(QStringLiteral("parameter_schema")).toObject();

    entry.useCount = object.value(QStringLiteral("use_count")).toInt(0);
    entry.successCount = object.value(QStringLiteral("success_count")).toInt(0);
    entry.failureCount = object.value(QStringLiteral("failure_count")).toInt(0);
    entry.version = object.value(QStringLiteral("version")).toInt(1);

    entry.createdAt = QDateTime::fromString(
        object.value(QStringLiteral("created_at")).toString(), Qt::ISODate);
    entry.updatedAt = QDateTime::fromString(
        object.value(QStringLiteral("updated_at")).toString(), Qt::ISODate);
    entry.lastUsedAt = QDateTime::fromString(
        object.value(QStringLiteral("last_used_at")).toString(), Qt::ISODate);

    return entry;
}
