//
// ContextBuilder
// 负责构造 AIBrain 调用 LLM 所需的系统提示与运行时上下文
//

#ifndef DESKTOP_PET_CONTEXT_BUILDER_H
#define DESKTOP_PET_CONTEXT_BUILDER_H

#include <QString>
#include <QStringList>

#include <optional>

#include "emotion/emotion_types.h"
#include "identity/identity_baseline.h"
#include "identity/persona_projector.h"
#include "prompt/prompt_template_types.h"

class ContextBuilder {
public:
    // 注入通用提示词模版与独立身份基线（按值）。未注入模版时使用内联兜底。
    void setPromptTemplate(const PromptTemplate& templ) { m_template = templ; m_templateSet = true; }
    void setIdentityBaseline(const IdentityBaseline& baseline) { m_identityBaseline = baseline; }

    QString buildSystemPrompt(
        const QString& petName,
        const std::optional<PersonaProjection>& projection = std::nullopt) const;
    QString buildRuntimeContext(const QString& petName,
                                const QString& reason,
                                const QString& currentState = QString(),
                                const QString& triggerTag = QString(),
                                const QStringList& allowedActions = QStringList(),
                                const std::optional<EmotionSnapshot>& emotion = std::nullopt) const;

private:
    QString buildStatisticsSummary(const QString& petName) const;
    QString appendFixedSafetyRules(const QString& prompt) const;
    QString inlineFallbackSystemPrompt(
        const QString& petName,
        const std::optional<PersonaProjection>& projection) const;

    PromptTemplate m_template;
    IdentityBaseline m_identityBaseline = IdentityBaseline::defaults();
    bool m_templateSet = false;
};

#endif // DESKTOP_PET_CONTEXT_BUILDER_H
