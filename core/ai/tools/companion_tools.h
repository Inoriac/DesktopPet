//
// 陪伴表达工具
//

#ifndef DESKTOP_PET_COMPANION_TOOLS_H
#define DESKTOP_PET_COMPANION_TOOLS_H

#include "../ai_tool.h"

#include <QDateTime>
#include <QStringList>

#include <functional>

class CompanionProactiveState {
public:
    static QString mode();
    static QDateTime updatedAt();
    static QStringList supportedModes();
    static bool setMode(const QString& mode, QString* errorMessage = nullptr);
};

class ShowChatBubbleTool : public AITool {
public:
    using Callback = std::function<void(const QString& text, int durationMs)>;

    explicit ShowChatBubbleTool(Callback callback);

    QJsonObject parameterSchema() const override;
    bool validate(const QJsonObject& params) const override;
    ToolResult execute(const QJsonObject& params) override;

private:
    Callback m_callback;
};

class NotifyUserTool : public AITool {
public:
    using Callback = std::function<void(const QString& title, const QString& message, int durationMs)>;

    explicit NotifyUserTool(Callback callback);

    QJsonObject parameterSchema() const override;
    bool validate(const QJsonObject& params) const override;
    ToolResult execute(const QJsonObject& params) override;

private:
    Callback m_callback;
};

class SetProactiveModeTool : public AITool {
public:
    using Callback = std::function<void(const QString& mode, int quietMinutes)>;

    explicit SetProactiveModeTool(Callback callback = {});

    QJsonObject parameterSchema() const override;
    bool validate(const QJsonObject& params) const override;
    ToolResult execute(const QJsonObject& params) override;

private:
    Callback m_callback;
};

#endif // DESKTOP_PET_COMPANION_TOOLS_H
