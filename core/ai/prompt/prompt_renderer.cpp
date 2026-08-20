//
// PromptRenderer — 实现
//

#include "prompt_renderer.h"

#include <QRegularExpression>

QString PromptRenderer::render(const QString& body, const QMap<QString, QString>& vars) {
    QString result = body;
    for (auto it = vars.constBegin(); it != vars.constEnd(); ++it) {
        result.replace(QStringLiteral("{{%1}}").arg(it.key()), redactSecrets(it.value()));
    }
    // 残留未替换的 {{slot}} 收敛为空（注意：技能系统的 {参数名} 是单括号，不会被误伤）。
    static const QRegularExpression leftover(QStringLiteral("\\{\\{[^}]+\\}\\}"));
    result.remove(leftover);
    return result;
}

QString PromptRenderer::redactSecrets(const QString& text) {
    QString sanitized = text;
    sanitized.replace(QStringLiteral("api_key"), QStringLiteral("api_[redacted]"), Qt::CaseInsensitive);
    sanitized.replace(QStringLiteral("password"), QStringLiteral("pass[redacted]"), Qt::CaseInsensitive);
    sanitized.replace(QStringLiteral("token"), QStringLiteral("tok[redacted]"), Qt::CaseInsensitive);
    sanitized.replace(QStringLiteral("secret"), QStringLiteral("sec[redacted]"), Qt::CaseInsensitive);
    return sanitized;
}
