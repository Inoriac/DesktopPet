//
// PromptTemplate — 序列化
//

#include "prompt_template_types.h"

QJsonObject PromptTemplate::toJson() const {
    QJsonObject obj;
    obj[QStringLiteral("id")] = id;
    obj[QStringLiteral("name")] = name;
    obj[QStringLiteral("description")] = description;
    obj[QStringLiteral("systemPromptBody")] = systemPromptBody;
    obj[QStringLiteral("slots")] = QJsonArray::fromStringList(slotNames);
    obj[QStringLiteral("version")] = version;
    if (createdAt.isValid()) {
        obj[QStringLiteral("createdAt")] = createdAt.toUTC().toString(Qt::ISODate);
    }
    if (updatedAt.isValid()) {
        obj[QStringLiteral("updatedAt")] = updatedAt.toUTC().toString(Qt::ISODate);
    }
    return obj;
}

PromptTemplate PromptTemplate::fromJson(const QJsonObject& object) {
    PromptTemplate t;
    t.id = object.value(QStringLiteral("id")).toString();
    t.name = object.value(QStringLiteral("name")).toString();
    t.description = object.value(QStringLiteral("description")).toString();
    t.systemPromptBody = object.value(QStringLiteral("systemPromptBody")).toString();

    const QJsonArray slotsArray = object.value(QStringLiteral("slots")).toArray();
    for (const QJsonValue& value : slotsArray) {
        const QString slot = value.toString().trimmed();
        if (!slot.isEmpty()) {
            t.slotNames.append(slot);
        }
    }

    t.version = object.value(QStringLiteral("version")).toInt(1);
    t.createdAt = QDateTime::fromString(object.value(QStringLiteral("createdAt")).toString(), Qt::ISODate);
    t.updatedAt = QDateTime::fromString(object.value(QStringLiteral("updatedAt")).toString(), Qt::ISODate);
    return t;
}
