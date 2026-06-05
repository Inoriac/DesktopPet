#include "memory_store.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>

QJsonObject MemoryEntry::toJson() const {
    QJsonArray tagArray;
    for (const QString& tag : tags) {
        tagArray.append(tag);
    }

    QJsonObject obj;
    obj["id"] = id;
    obj["type"] = memoryTypeToString(type);
    obj["key"] = key;
    obj["value"] = value;
    obj["tags"] = tagArray;
    obj["created_at"] = createdAt.toString(Qt::ISODate);
    obj["updated_at"] = updatedAt.toString(Qt::ISODate);
    return obj;
}

MemoryEntry MemoryEntry::fromJson(const QJsonObject& object) {
    MemoryEntry entry;
    entry.id = object.value("id").toString();
    entry.type = memoryTypeFromString(object.value("type").toString());
    entry.key = object.value("key").toString();
    entry.value = object.value("value");
    for (const QJsonValue& tag : object.value("tags").toArray()) {
        entry.tags.append(tag.toString());
    }
    entry.createdAt = QDateTime::fromString(object.value("created_at").toString(), Qt::ISODate);
    entry.updatedAt = QDateTime::fromString(object.value("updated_at").toString(), Qt::ISODate);
    return entry;
}

void MemoryStore::setStoragePath(const QString& memoryFilePath) {
    m_memoryFilePath = memoryFilePath;
}

bool MemoryStore::load(QString* errorMessage) {
    QFile file(m_memoryFilePath);
    if (!file.exists()) {
        m_entries.clear();
        return true;
    }

    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage) *errorMessage = file.errorString();
        return false;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray()) {
        if (errorMessage) *errorMessage = "memory file is not a JSON array";
        return false;
    }

    m_entries.clear();
    for (const QJsonValue& value : doc.array()) {
        if (value.isObject()) {
            m_entries.append(MemoryEntry::fromJson(value.toObject()));
        }
    }
    return true;
}

bool MemoryStore::save(QString* errorMessage) const {
    const QFileInfo info(m_memoryFilePath);
    if (!info.dir().exists() && !QDir().mkpath(info.dir().path())) {
        if (errorMessage) *errorMessage = "failed to create memory directory";
        return false;
    }

    QJsonArray array;
    for (const MemoryEntry& entry : m_entries) {
        array.append(entry.toJson());
    }

    QFile file(m_memoryFilePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage) *errorMessage = file.errorString();
        return false;
    }

    file.write(QJsonDocument(array).toJson(QJsonDocument::Indented));
    return true;
}

MemoryEntry MemoryStore::add(MemoryType type,
                             const QString& key,
                             const QJsonValue& value,
                             const QStringList& tags) {
    MemoryEntry entry;
    entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    entry.type = type;
    entry.key = key;
    entry.value = value;
    entry.tags = tags;
    entry.createdAt = QDateTime::currentDateTimeUtc();
    entry.updatedAt = entry.createdAt;
    m_entries.append(entry);
    return entry;
}

QList<MemoryEntry> MemoryStore::recent(MemoryType type, int limit) const {
    QList<MemoryEntry> result;
    for (auto it = m_entries.crbegin(); it != m_entries.crend() && result.size() < limit; ++it) {
        if (it->type == type) {
            result.append(*it);
        }
    }
    return result;
}

QList<MemoryEntry> MemoryStore::findByTag(const QString& tag, int limit) const {
    QList<MemoryEntry> result;
    for (auto it = m_entries.crbegin(); it != m_entries.crend() && result.size() < limit; ++it) {
        if (it->tags.contains(tag)) {
            result.append(*it);
        }
    }
    return result;
}

QStringList MemoryStore::summaryForContext(int limit) const {
    QStringList result;
    for (auto it = m_entries.crbegin(); it != m_entries.crend() && result.size() < limit; ++it) {
        const QString valueText = it->value.isString()
                                  ? it->value.toString()
                                  : QString::fromUtf8(QJsonDocument(QJsonObject{{"value", it->value}}).toJson(QJsonDocument::Compact));
        result.append(QString("[%1] %2=%3").arg(memoryTypeToString(it->type), it->key, valueText));
    }
    return result;
}

void MemoryStore::clear() {
    m_entries.clear();
}