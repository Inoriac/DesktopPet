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
#include "prompt/prompt_template_types.h"
#include "pet_personality.h"

class ContextBuilder {
public:
    // 注入通用提示词模版与性格预设（按值）。未注入时 buildSystemPrompt 回退内联兜底字面量。
    void setPromptTemplate(const PromptTemplate& templ) { m_template = templ; m_templateSet = true; }
    void setPersona(const PetPersonality& persona) { m_persona = persona; m_personaSet = true; }

    QString buildSystemPrompt(const QString& petName) const;
    QString buildRuntimeContext(const QString& petName,
                                const QString& reason,
                                const QString& currentState = QString(),
                                const QString& triggerTag = QString(),
                                const QStringList& allowedActions = QStringList(),
                                const std::optional<EmotionSnapshot>& emotion = std::nullopt) const;

private:
    QString buildStatisticsSummary(const QString& petName) const;
    // 未注入模版时的零回归兜底：与改造前的硬编码系统提示词逐字一致。
    QString inlineFallbackSystemPrompt(const QString& petName) const;

    PromptTemplate m_template;
    PetPersonality m_persona;
    bool m_templateSet = false;
    bool m_personaSet = false;
};

#endif // DESKTOP_PET_CONTEXT_BUILDER_H
