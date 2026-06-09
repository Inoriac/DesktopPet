//
// 陪伴表达工具
//

#include "companion_tools.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace {
constexpr int kDefaultBubbleDurationMs = 8000;
constexpr int kMinDurationMs = 1000;
constexpr int kMaxDurationMs = 30000;
constexpr int kMaxBubbleTextLength = 240;
constexpr int kMaxTitleLength = 60;
constexpr int kMaxMessageLength = 240;

QString stateFilePath() {
    const QString configDir = QDir::current().filePath("config");
    QDir().mkpath(configDir);
    return QDir(configDir).filePath("agent_proactive_state.json");
}

QJsonObject readState() {
    QFile file(stateFilePath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    return doc.isObject() ? doc.object() : QJsonObject{};
}

bool writeState(const QJsonObject& state) {
    QSaveFile file(stateFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    file.write(QJsonDocument(state).toJson(QJsonDocument::Indented));
    return file.commit();
}

QString normalizeMode(const QString& mode) {
    QString normalized = mode.trimmed().toLower();
    if (normalized == "安静") return "quiet";
    if (normalized == "普通" || normalized == "正常") return "normal";
    if (normalized == "活泼") return "lively";
    if (normalized == "专注" || normalized == "工作" || normalized == "勿扰") return "focus";
    return normalized;
}

int boundedDuration(int durationMs) {
    if (durationMs <= 0) {
        return kDefaultBubbleDurationMs;
    }
    return qBound(kMinDurationMs, durationMs, kMaxDurationMs);
}

QString clippedText(const QString& text, int maxLength) {
    QString trimmed = text.trimmed();
    if (trimmed.size() > maxLength) {
        trimmed = trimmed.left(maxLength);
    }
    return trimmed;
}
}

QString CompanionProactiveState::mode() {
    return readState().value("mode").toString("normal");
}

QDateTime CompanionProactiveState::updatedAt() {
    const QString value = readState().value("updated_at").toString();
    const QDateTime parsed = QDateTime::fromString(value, Qt::ISODate);
    return parsed.isValid() ? parsed : QDateTime{};
}

QStringList CompanionProactiveState::supportedModes() {
    return {"quiet", "normal", "lively", "focus"};
}

bool CompanionProactiveState::setMode(const QString& mode, QString* errorMessage) {
    const QString normalized = normalizeMode(mode);
    if (!supportedModes().contains(normalized)) {
        if (errorMessage) {
            *errorMessage = QString("不支持的主动模式: %1").arg(mode);
        }
        return false;
    }

    QJsonObject state = readState();
    state["mode"] = normalized;
    state["updated_at"] = QDateTime::currentDateTime().toString(Qt::ISODate);

    if (!writeState(state)) {
        if (errorMessage) {
            *errorMessage = "主动模式保存失败";
        }
        return false;
    }

    return true;
}

ShowChatBubbleTool::ShowChatBubbleTool(Callback callback)
    : AITool(
          "show_chat_bubble",
          "在桌宠旁边显示一段短气泡文本。适合陪伴表达、到点提醒和轻量提示。文本应简短自然，不要包含隐私信息。",
          ToolCategory::Action)
    , m_callback(std::move(callback)) {}

QJsonObject ShowChatBubbleTool::parameterSchema() const {
    QJsonObject schema;
    schema["type"] = "object";

    QJsonObject properties;
    QJsonObject text;
    text["type"] = "string";
    text["description"] = "要显示的气泡文本，建议 80 字以内";
    properties["text"] = text;

    QJsonObject durationMs;
    durationMs["type"] = "integer";
    durationMs["description"] = "显示时长（毫秒），默认 8000，范围 1000-30000";
    durationMs["default"] = kDefaultBubbleDurationMs;
    properties["duration_ms"] = durationMs;

    schema["properties"] = properties;
    QJsonArray required;
    required.append("text");
    schema["required"] = required;
    return schema;
}

bool ShowChatBubbleTool::validate(const QJsonObject& params) const {
    return params.contains("text") && !params.value("text").toString().trimmed().isEmpty();
}

ToolResult ShowChatBubbleTool::execute(const QJsonObject& params) {
    if (!m_callback) {
        return ToolResult::fail("气泡显示回调未配置");
    }

    const QString text = clippedText(params.value("text").toString(), kMaxBubbleTextLength);
    const int durationMs = boundedDuration(params.value("duration_ms").toInt(kDefaultBubbleDurationMs));
    m_callback(text, durationMs);

    QJsonObject result;
    result["text"] = text;
    result["duration_ms"] = durationMs;
    result["shown"] = true;
    return ToolResult::ok(result);
}

NotifyUserTool::NotifyUserTool(Callback callback)
    : AITool(
          "notify_user",
          "向用户发送一条低风险提醒。优先使用桌宠气泡；如果以后接入系统托盘通知，可复用该工具。",
          ToolCategory::Action)
    , m_callback(std::move(callback)) {}

QJsonObject NotifyUserTool::parameterSchema() const {
    QJsonObject schema;
    schema["type"] = "object";

    QJsonObject properties;
    QJsonObject title;
    title["type"] = "string";
    title["description"] = "通知标题，建议 20 字以内";
    title["default"] = "桌宠提醒";
    properties["title"] = title;

    QJsonObject message;
    message["type"] = "string";
    message["description"] = "通知正文，建议 80 字以内";
    properties["message"] = message;

    QJsonObject durationMs;
    durationMs["type"] = "integer";
    durationMs["description"] = "显示时长（毫秒），默认 8000";
    durationMs["default"] = kDefaultBubbleDurationMs;
    properties["duration_ms"] = durationMs;

    schema["properties"] = properties;
    QJsonArray required;
    required.append("message");
    schema["required"] = required;
    return schema;
}

bool NotifyUserTool::validate(const QJsonObject& params) const {
    return params.contains("message") && !params.value("message").toString().trimmed().isEmpty();
}

ToolResult NotifyUserTool::execute(const QJsonObject& params) {
    if (!m_callback) {
        return ToolResult::fail("通知回调未配置");
    }

    const QString title = clippedText(params.value("title").toString("桌宠提醒"), kMaxTitleLength);
    const QString message = clippedText(params.value("message").toString(), kMaxMessageLength);
    const int durationMs = boundedDuration(params.value("duration_ms").toInt(kDefaultBubbleDurationMs));
    m_callback(title, message, durationMs);

    QJsonObject result;
    result["title"] = title;
    result["message"] = message;
    result["duration_ms"] = durationMs;
    result["notified"] = true;
    return ToolResult::ok(result);
}

SetProactiveModeTool::SetProactiveModeTool(Callback callback)
    : AITool(
          "set_proactive_mode",
          "设置桌宠主动程度。支持 quiet(安静)、normal(普通)、lively(活泼)、focus(专注/勿扰)。该设置会轻量持久化。",
          ToolCategory::Action)
    , m_callback(std::move(callback)) {}

QJsonObject SetProactiveModeTool::parameterSchema() const {
    QJsonObject schema;
    schema["type"] = "object";

    QJsonObject properties;
    QJsonObject mode;
    mode["type"] = "string";
    mode["description"] = "主动模式：quiet、normal、lively、focus；也接受中文：安静、普通、活泼、专注、勿扰";
    QJsonArray modeEnum;
    modeEnum.append("quiet");
    modeEnum.append("normal");
    modeEnum.append("lively");
    modeEnum.append("focus");
    mode["enum"] = modeEnum;
    properties["mode"] = mode;

    QJsonObject quietMinutes;
    quietMinutes["type"] = "integer";
    quietMinutes["description"] = "可选：临时安静分钟数，第一版仅返回该值，调度器后续可消费";
    quietMinutes["default"] = 0;
    properties["quiet_minutes"] = quietMinutes;

    schema["properties"] = properties;
    QJsonArray required;
    required.append("mode");
    schema["required"] = required;
    return schema;
}

bool SetProactiveModeTool::validate(const QJsonObject& params) const {
    if (!params.contains("mode")) {
        return false;
    }
    return CompanionProactiveState::supportedModes().contains(normalizeMode(params.value("mode").toString()));
}

ToolResult SetProactiveModeTool::execute(const QJsonObject& params) {
    const QString mode = normalizeMode(params.value("mode").toString());
    QString errorMessage;
    if (!CompanionProactiveState::setMode(mode, &errorMessage)) {
        return ToolResult::fail(errorMessage);
    }

    const int quietMinutes = qMax(0, params.value("quiet_minutes").toInt(0));
    if (m_callback) {
        m_callback(mode, quietMinutes);
    }

    QJsonObject result;
    result["mode"] = mode;
    result["quiet_minutes"] = quietMinutes;
    result["updated_at"] = CompanionProactiveState::updatedAt().toString(Qt::ISODate);
    return ToolResult::ok(result);
}
