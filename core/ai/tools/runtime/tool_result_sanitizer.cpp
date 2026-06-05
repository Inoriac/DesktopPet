#include "tool_result_sanitizer.h"

#include <QtGlobal>

void ToolResultSanitizer::setMaxStringLength(int length) {
    m_maxStringLength = qMax(256, length);
}

ToolResult ToolResultSanitizer::sanitize(const ToolResult& result) const {
    ToolResult sanitized = result;
    sanitized.data = sanitizeObject(result.data);
    if (!sanitized.errorMessage.isEmpty()) {
        sanitized.errorMessage = sanitizeString("error", sanitized.errorMessage);
    }
    return sanitized;
}

QJsonObject ToolResultSanitizer::sanitizeObject(const QJsonObject& object) const {
    QJsonObject sanitized;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (isSensitiveKey(it.key())) {
            sanitized[it.key()] = "[REDACTED]";
            continue;
        }

        if (it.value().isObject()) {
            sanitized[it.key()] = sanitizeObject(it.value().toObject());
        } else if (it.value().isArray()) {
            sanitized[it.key()] = sanitizeArray(it.value().toArray());
        } else if (it.value().isString()) {
            sanitized[it.key()] = sanitizeString(it.key(), it.value().toString());
        } else {
            sanitized[it.key()] = it.value();
        }
    }
    return sanitized;
}

QJsonArray ToolResultSanitizer::sanitizeArray(const QJsonArray& array) const {
    QJsonArray sanitized;
    for (const QJsonValue& value : array) {
        if (value.isObject()) {
            sanitized.append(sanitizeObject(value.toObject()));
        } else if (value.isArray()) {
            sanitized.append(sanitizeArray(value.toArray()));
        } else if (value.isString()) {
            sanitized.append(sanitizeString(QString(), value.toString()));
        } else {
            sanitized.append(value);
        }
    }
    return sanitized;
}

QString ToolResultSanitizer::sanitizeString(const QString& key, const QString& value) const {
    if (isSensitiveKey(key)) {
        return "[REDACTED]";
    }

    if (value.size() <= m_maxStringLength) {
        return value;
    }

    return value.left(m_maxStringLength) + QString("... [truncated %1 chars]").arg(value.size() - m_maxStringLength);
}

bool ToolResultSanitizer::isSensitiveKey(const QString& key) const {
    const QString normalized = key.toLower();
    return normalized.contains("api_key")
        || normalized.contains("apikey")
        || normalized.contains("token")
        || normalized.contains("password")
        || normalized.contains("secret")
        || normalized.contains("credential")
        || normalized.contains("authorization");
}