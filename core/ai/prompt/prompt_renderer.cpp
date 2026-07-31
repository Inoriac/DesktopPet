//
// PromptRenderer — 实现
//

#include "prompt_renderer.h"

#include <QRegularExpression>

#include "pet_personality.h"

QString PromptRenderer::render(const QString& body, const QMap<QString, QString>& vars) {
    QString result = body;
    for (auto it = vars.constBegin(); it != vars.constEnd(); ++it) {
        result.replace(QStringLiteral("{{%1}}").arg(it.key()), it.value());
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

QMap<QString, QString> PromptRenderer::buildVariables(const PetPersonality& persona, const QString& petName) {
    QMap<QString, QString> vars;

    const QString safePetName = petName.isEmpty() ? QStringLiteral("桌宠") : petName;
    vars[QStringLiteral("pet_name")] = safePetName;
    vars[QStringLiteral("gender")] = persona.gender.isEmpty() ? QStringLiteral("neutral") : persona.gender;

    // 性格特质 → "性格：平和、体贴。"
    if (!persona.traits.isEmpty()) {
        vars[QStringLiteral("persona_traits")] =
            QStringLiteral("性格：%1。").arg(persona.traits.join(QStringLiteral("、")));
    } else {
        vars[QStringLiteral("persona_traits")] = QString();
    }

    // tone 与 speaking_style 是自由文本，过脱敏后注入。
    vars[QStringLiteral("tone")] = redactSecrets(persona.tone);
    vars[QStringLiteral("speaking_style")] = redactSecrets(persona.speakingStyle);

    // 口头禅 → "口头禅：诶嘿、哎呀我又忘了。"
    if (!persona.catchphrases.isEmpty()) {
        vars[QStringLiteral("catchphrases")] =
            QStringLiteral("口头禅：%1。").arg(persona.catchphrases.join(QStringLiteral("、")));
    } else {
        vars[QStringLiteral("catchphrases")] = QString();
    }

    // 追加指令 → 换行拼接的额外要求块
    if (!persona.extraDirectives.isEmpty()) {
        QStringList redacted;
        redacted.reserve(persona.extraDirectives.size());
        for (const QString& directive : persona.extraDirectives) {
            redacted.append(redactSecrets(directive));
        }
        vars[QStringLiteral("extra_directives")] =
            QStringLiteral("\n额外要求：\n%1").arg(redacted.join(QStringLiteral("\n")));
    } else {
        vars[QStringLiteral("extra_directives")] = QString();
    }

    return vars;
}
