//
// AI 调用日志记录器
// 将每次请求与回复写入独立日志文件（JSONL）
//

#ifndef DESKTOP_PET_AI_CALL_LOGGER_H
#define DESKTOP_PET_AI_CALL_LOGGER_H

#include <QList>
#include <QJsonObject>
#include <QString>

#include "ai_types.h"

class AiCallLogger {
public:
    explicit AiCallLogger(const QString& logFilePath = "log/ai_calls.jsonl");

    void setLogFilePath(const QString& path);

    void logRequest(const QString& requestId,
                    const QString& petName,
                    const QString& reason,
                    const QString& triggerTag,
                    int toolRound,
                    const QList<ChatMessage>& messages,
                    const QJsonArray& tools);

    void logResponse(const QString& requestId,
                     const QString& petName,
                     bool success,
                     const LlmResponse& response,
                     const QString& errorMessage);

private:
    QString m_logFilePath;

    bool appendLine(const QJsonObject& lineObject) const;
    static QJsonObject messageToJson(const ChatMessage& msg);
    static QJsonObject toolCallToJson(const LlmToolCall& call);
};

#endif // DESKTOP_PET_AI_CALL_LOGGER_H
