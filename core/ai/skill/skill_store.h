//
// SkillStore — CRUD and JSON file persistence for skills
//

#ifndef DESKTOP_PET_SKILL_STORE_H
#define DESKTOP_PET_SKILL_STORE_H

#include <QList>
#include <QString>

#include "skill_types.h"

class SkillStore {
public:
    void setStoragePath(const QString& directoryPath);
    const QString& storagePath() const { return m_storagePath; }

    bool load(QString* errorMessage = nullptr);
    bool saveEntry(const SkillEntry& entry, QString* errorMessage = nullptr) const;
    bool removeFile(const QString& id, QString* errorMessage = nullptr) const;

    SkillEntry add(const SkillEntry& entry);
    bool update(const SkillEntry& entry);
    bool remove(const QString& id);

    bool recordOutcome(const QString& id, bool success);

    const QList<SkillEntry>& all() const { return m_entries; }
    const SkillEntry* findById(const QString& id) const;
    SkillEntry* findById(const QString& id);
    const SkillEntry* findByName(const QString& name) const;
    QList<SkillEntry> findByDomain(const QString& domain) const;
    QList<SkillEntry> findByTag(const QString& tag) const;

    int count() const { return m_entries.size(); }

private:
    QString filePathForId(const QString& id) const;

    QString m_storagePath = QStringLiteral("runtime/skills");
    QList<SkillEntry> m_entries;
};

#endif // DESKTOP_PET_SKILL_STORE_H
