//
// PromptTemplateStore — 实现
//

#include "prompt_template_store.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonDocument>

void PromptTemplateStore::setStoragePath(const QString& directoryPath) {
    m_storagePath = directoryPath;
}

bool PromptTemplateStore::load(QString* errorMessage) {
    m_entries.clear();

    QDir dir(m_storagePath);
    if (!dir.exists()) {
        // 内置模版目录缺失不视为致命：调用方回退到 ContextBuilder 内联兜底模版。
        if (errorMessage) {
            *errorMessage = QStringLiteral("提示词模版目录不存在: %1").arg(m_storagePath);
        }
        return false;
    }

    const QStringList files = dir.entryList({QStringLiteral("*.json")}, QDir::Files);
    for (const QString& fileName : files) {
        QFile file(dir.filePath(fileName));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            continue;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        if (!doc.isObject()) {
            continue;
        }

        PromptTemplate entry = PromptTemplate::fromJson(doc.object());
        if (entry.id.isEmpty() || entry.name.isEmpty() || entry.systemPromptBody.isEmpty()) {
            continue;
        }

        m_entries.append(entry);
    }

    return true;
}

const PromptTemplate* PromptTemplateStore::findById(const QString& id) const {
    for (const PromptTemplate& entry : m_entries) {
        if (entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

const PromptTemplate* PromptTemplateStore::findByName(const QString& name) const {
    const QString normalized = name.trimmed().toLower();
    for (const PromptTemplate& entry : m_entries) {
        if (entry.name.trimmed().toLower() == normalized
            || entry.id.trimmed().toLower() == normalized) {
            return &entry;
        }
    }
    return nullptr;
}
