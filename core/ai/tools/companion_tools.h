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
    static QString mode(const QString& path = {}, const QDateTime& now = QDateTime::currentDateTimeUtc());
    static QDateTime updatedAt(const QString& path = {});
    static QStringList supportedModes();
    static bool setMode(const QString& mode, QString* errorMessage = nullptr,
                        int quietMinutes = 0, const QString& path = {},
                        const QDateTime& now = QDateTime::currentDateTimeUtc());
};

class ShowChatBubbleTool : public AITool {
public:
    // True means the UI accepted the text for display. A busy or unavailable
    // surface must return false so scheduled reminders can retry.
    using Callback = std::function<bool(const QString& text, int durationMs)>;

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

    explicit SetProactiveModeTool(Callback callback = {}, QString statePath = {});

    QJsonObject parameterSchema() const override;
    bool validate(const QJsonObject& params) const override;
    ToolResult execute(const QJsonObject& params) override;

private:
    Callback m_callback;
    QString m_statePath;
};

#endif // DESKTOP_PET_COMPANION_TOOLS_H
