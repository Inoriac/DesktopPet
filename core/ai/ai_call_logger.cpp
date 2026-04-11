//
// AI 调用日志记录器实现
//

#include "ai_call_logger.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

AiCallLogger::AiCallLogger(const QString& logFilePath)
    : m_logFilePath(logFilePath) {}

void AiCallLogger::setLogFilePath(const QString& path) {
    m_logFilePath = path;
}

void AiCallLogger::logRequest(const QString& requestId,
                              const QString& petName,
                              const QString& reason,
                              const QString& triggerTag,
                              int toolRound,
                              const QList<ChatMessage>& messages,
                              const QJsonArray& tools) {
    QJsonArray msgArr;
    for (const ChatMessage& msg : messages) {
        msgArr.append(messageToJson(msg));
    }

    QJsonObject obj;
    obj["type"] = "request";
    obj["request_id"] = requestId;
    obj["timestamp"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    obj["pet_name"] = petName;
    obj["reason"] = reason;
    obj["trigger_tag"] = triggerTag;
    obj["tool_round"] = toolRound;
    obj["messages"] = msgArr;
    obj["tools"] = tools;

    appendLine(obj);
}

void AiCallLogger::logResponse(const QString& requestId,
                               const QString& petName,
                               bool success,
                               const LlmResponse& response,
                               const QString& errorMessage) {
    QJsonArray toolCallsArr;
    for (const LlmToolCall& call : response.toolCalls) {
        toolCallsArr.append(toolCallToJson(call));
    }

    QJsonObject usageObj;
    usageObj["prompt_tokens"] = response.usage.promptTokens;
    usageObj["completion_tokens"] = response.usage.completionTokens;
    usageObj["total_tokens"] = response.usage.totalTokens;
    usageObj["reasoning_tokens"] = response.usage.reasoningTokens;
    usageObj["cached_tokens"] = response.usage.cachedTokens;
    usageObj["prompt_cache_hit_tokens"] = response.usage.promptCacheHitTokens;
    usageObj["prompt_cache_miss_tokens"] = response.usage.promptCacheMissTokens;

    QJsonObject obj;
    obj["type"] = "response";
    obj["request_id"] = requestId;
    obj["timestamp"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    obj["pet_name"] = petName;
    obj["success"] = success;
    obj["error"] = errorMessage;
    obj["response_id"] = response.id;
    obj["model"] = response.model;
    obj["created"] = response.created;
    obj["finish_reason"] = response.finishReason;
    obj["content"] = response.content;
    obj["reasoning_content"] = response.reasoningContent;
    obj["tool_calls"] = toolCallsArr;
    obj["usage"] = usageObj;

    appendLine(obj);
}

bool AiCallLogger::appendLine(const QJsonObject& lineObject) const {
    if (m_logFilePath.isEmpty()) {
        return false;
    }

    QFileInfo fi(m_logFilePath);
    QDir dir = fi.dir();
    if (!dir.exists()) {
        dir.mkpath(".");
    }

    QFile file(m_logFilePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return false;
    }

    const QByteArray line = QJsonDocument(lineObject).toJson(QJsonDocument::Compact);
    file.write(line);
    file.write("\n");
    file.close();
    return true;
}

QJsonObject AiCallLogger::messageToJson(const ChatMessage& msg) {
    QJsonObject obj;
    obj["role"] = msg.role;
    obj["content"] = msg.content;
    if (!msg.name.isEmpty()) {
        obj["name"] = msg.name;
    }
    if (!msg.toolCallId.isEmpty()) {
        obj["tool_call_id"] = msg.toolCallId;
    }
    if (!msg.toolCalls.isEmpty()) {
        obj["tool_calls"] = msg.toolCalls;
    }
    return obj;
}

QJsonObject AiCallLogger::toolCallToJson(const LlmToolCall& call) {
    QJsonObject obj;
    obj["id"] = call.id;
    obj["type"] = call.type;
    obj["name"] = call.name;
    obj["arguments"] = call.arguments;
    return obj;
}
