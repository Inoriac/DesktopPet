//
// PromptTemplateStore — 通用提示词模版的只读加载器
// 仿 SkillStore 的目录扫描模式：扫 config/prompts/*.json，物化为 PromptTemplate。
// 本轮为只读内置模版（不做 save/CRUD），后续阶段可扩展为可写 store。
//

#ifndef DESKTOP_PET_PROMPT_TEMPLATE_STORE_H
#define DESKTOP_PET_PROMPT_TEMPLATE_STORE_H

#include <QList>
#include <QString>

#include "prompt_template_types.h"

class PromptTemplateStore {
public:
    void setStoragePath(const QString& directoryPath);
    const QString& storagePath() const { return m_storagePath; }

    // 扫描目录下 *.json，解析为 PromptTemplate。目录缺失返回 false（调用方回退内联兜底）。
    bool load(QString* errorMessage = nullptr);

    const QList<PromptTemplate>& all() const { return m_entries; }
    const PromptTemplate* findById(const QString& id) const;
    // 按 name 或 id 匹配（大小写不敏感），便于用 "default" 一类标识检索。
    const PromptTemplate* findByName(const QString& name) const;

    int count() const { return m_entries.size(); }

private:
    QString m_storagePath = QStringLiteral("config/prompts");
    QList<PromptTemplate> m_entries;
};

#endif // DESKTOP_PET_PROMPT_TEMPLATE_STORE_H
