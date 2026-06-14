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

    void setMaxPayloadLength(int length);
    int maxPayloadLength() const { return m_maxPayloadLength; }

    ToolResult sanitize(const ToolResult& result) const;
    QString toPayload(const ToolResult& result) const;
    QJsonObject sanitizeObject(const QJsonObject& object) const;
    QJsonArray sanitizeArray(const QJsonArray& array) const;

private:
    QString sanitizeString(const QString& key, const QString& value) const;
    QString trimPayload(const QString& payload, bool success) const;
    bool isSensitiveKey(const QString& key) const;

private:
    int m_maxStringLength = 2000;
    int m_maxPayloadLength = 6000;
};

#endif // DESKTOP_PET_TOOL_RESULT_SANITIZER_H