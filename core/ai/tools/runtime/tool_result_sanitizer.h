#ifndef DESKTOP_PET_TOOL_RESULT_SANITIZER_H
#define DESKTOP_PET_TOOL_RESULT_SANITIZER_H

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include "ai_types.h"

class ToolResultSanitizer {
public:
    void setMaxStringLength(int length);
    int maxStringLength() const { return m_maxStringLength; }

    ToolResult sanitize(const ToolResult& result) const;
    QJsonObject sanitizeObject(const QJsonObject& object) const;
    QJsonArray sanitizeArray(const QJsonArray& array) const;

private:
    QString sanitizeString(const QString& key, const QString& value) const;
    bool isSensitiveKey(const QString& key) const;

private:
    int m_maxStringLength = 2000;
};

#endif // DESKTOP_PET_TOOL_RESULT_SANITIZER_H