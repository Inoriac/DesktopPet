//
// Created by Inoriac on 2026/4/8.
//

#ifndef DESKTOP_PET_AI_TYPES_H
#define DESKTOP_PET_AI_TYPES_H

#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>

// tool的类别标记
enum class ToolCategory {
    Query,      // 只读查询
    Action      // 有副作用的操作
};

struct ToolResult {
    bool success = false;
    QJsonObject data;
    QString errorMessage;

    // 快捷构造：成功
    static ToolResult ok(const QJsonObject& resultData = {}) {
        return {true, resultData, ""};
    }

    // 快捷构造：成功
    static ToolResult fail(const QString& error) {
        return {false, {}, error};
    }

    QJsonObject toJson() const {
        QJsonObject obj;
        obj["success"] = success;
        if (success) {
            obj["data"] = data;
        } else {
            obj["error"] = errorMessage;
        }
        return obj;
    }
};

#endif //DESKTOP_PET_AI_TYPES_H