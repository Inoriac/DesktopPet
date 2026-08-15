//
// SkillStore — CRUD and JSON file persistence
//

#include "skill_store.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUuid>
#include <utility>

void SkillStore::setStoragePath(const QString& directoryPath) {
    m_storagePath = directoryPath;
}

bool SkillStore::load(QString* errorMessage) {
    QDir dir(m_storagePath);
    if (!dir.exists()) {
        if (!dir.mkpath(QStringLiteral("."))) {
            if (errorMessage) *errorMessage = QStringLiteral("无法创建目录: %1").arg(m_storagePath);
            return false;
        }
        m_entries.clear();
        return true;
    }

    QList<SkillEntry> loadedEntries;
    const QStringList files = dir.entryList({QStringLiteral("*.json")}, QDir::Files);
    for (const QString& fileName : files) {
        QFile file(dir.filePath(fileName));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) continue;

        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        if (!doc.isObject()) continue;

        SkillEntry entry = SkillEntry::fromJson(doc.object());
        if (entry.id.isEmpty() || entry.name.isEmpty()) continue;

        loadedEntries.append(entry);
    }

    m_entries = std::move(loadedEntries);
    return true;
}

bool SkillStore::saveEntry(const SkillEntry& entry, QString* errorMessage) const {
    QDir dir(m_storagePath);
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        if (errorMessage) *errorMessage = QStringLiteral("无法创建目录: %1").arg(m_storagePath);
        return false;
    }

    QSaveFile file(filePathForId(entry.id));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (errorMessage) *errorMessage = QStringLiteral("无法写入文件: %1").arg(file.fileName());
        return false;
    }

    const QByteArray payload = QJsonDocument(entry.toJson()).toJson(QJsonDocument::Indented);
    if (file.write(payload) != payload.size()) {
        if (errorMessage) *errorMessage = QStringLiteral("Failed to write file: %1").arg(file.fileName());
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        if (errorMessage) *errorMessage = QStringLiteral("文件提交失败: %1").arg(file.fileName());
        return false;
    }

    return true;
}

bool SkillStore::removeFile(const QString& id, QString* errorMessage) const {
    const QString path = filePathForId(id);
    if (!QFile::exists(path)) return true;

    if (!QFile::remove(path)) {
        if (errorMessage) *errorMessage = QStringLiteral("无法删除文件: %1").arg(path);
        return false;
    }
    return true;
}

SkillEntry SkillStore::add(const SkillEntry& entry) {
    SkillEntry newEntry = entry;
    if (newEntry.id.isEmpty()) {
        newEntry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }

    const QDateTime now = QDateTime::currentDateTimeUtc();
    newEntry.createdAt = now;
    newEntry.updatedAt = now;
    newEntry.version = 1;

    if (!saveEntry(newEntry)) {
        return {};
    }
    m_entries.append(newEntry);
    return newEntry;
}

bool SkillStore::update(const SkillEntry& entry) {
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].id == entry.id) {
            SkillEntry updated = entry;
            updated.updatedAt = QDateTime::currentDateTimeUtc();
            updated.version = m_entries[i].version + 1;
            if (!saveEntry(updated)) {
                return false;
            }
            m_entries[i] = updated;
            return true;
        }
    }
    return false;
}

bool SkillStore::remove(const QString& id) {
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].id == id) {
            if (!removeFile(id)) {
                return false;
            }
            m_entries.removeAt(i);
            return true;
        }
    }
    return false;
}

bool SkillStore::recordOutcome(const QString& id, bool success) {
    SkillEntry* existing = findById(id);
    if (!existing) return false;
    SkillEntry updated = *existing;

    updated.useCount++;
    if (success) {
        updated.successCount++;
    } else {
        updated.failureCount++;
    }
    updated.lastUsedAt = QDateTime::currentDateTimeUtc();
    updated.updatedAt = updated.lastUsedAt;

    if (!saveEntry(updated)) {
        return false;
    }
    *existing = updated;
    return true;
}

const SkillEntry* SkillStore::findById(const QString& id) const {
    for (const SkillEntry& entry : m_entries) {
        if (entry.id == id) return &entry;
    }
    return nullptr;
}

SkillEntry* SkillStore::findById(const QString& id) {
    for (SkillEntry& entry : m_entries) {
        if (entry.id == id) return &entry;
    }
    return nullptr;
}

const SkillEntry* SkillStore::findByName(const QString& name) const {
    const QString normalized = name.trimmed().toLower();
    for (const SkillEntry& entry : m_entries) {
        if (entry.name.trimmed().toLower() == normalized) return &entry;
    }
    return nullptr;
}

QList<SkillEntry> SkillStore::findByDomain(const QString& domain) const {
    QList<SkillEntry> result;
    const QString normalized = domain.trimmed().toLower();
    for (const SkillEntry& entry : m_entries) {
        if (entry.domain.trimmed().toLower() == normalized) {
            result.append(entry);
        }
    }
    return result;
}

QList<SkillEntry> SkillStore::findByTag(const QString& tag) const {
    QList<SkillEntry> result;
    for (const SkillEntry& entry : m_entries) {
        if (entry.tags.contains(tag, Qt::CaseInsensitive)) {
            result.append(entry);
        }
    }
    return result;
}

QString SkillStore::filePathForId(const QString& id) const {
    return QDir(m_storagePath).filePath(id + QStringLiteral(".json"));
}
