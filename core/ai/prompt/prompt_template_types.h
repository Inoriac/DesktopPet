//
// PromptTemplate — 通用系统提示词模版的类型定义
// 仿 skill_types.h：一个模版=一份 JSON，带 systemPromptBody 与 slot 声明。
//

#ifndef DESKTOP_PET_PROMPT_TEMPLATE_TYPES_H
#define DESKTOP_PET_PROMPT_TEMPLATE_TYPES_H

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

struct PromptTemplate {
    QString id;
    QString name;
    QString description;
    QString systemPromptBody;   // 含 {{slot}} 占位的模版正文
    QStringList slotNames;       // 声明的 slot 名，便于校验/文档（字段名避开 Qt 的 slots 宏）
    int version = 1;
    QDateTime createdAt;
    QDateTime updatedAt;

    QJsonObject toJson() const;
    static PromptTemplate fromJson(const QJsonObject& object);
};

#endif // DESKTOP_PET_PROMPT_TEMPLATE_TYPES_H
