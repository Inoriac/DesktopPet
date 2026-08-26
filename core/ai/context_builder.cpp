//
// ContextBuilder implementation
//

#include "context_builder.h"

#include <QDateTime>

#include <algorithm>
#include <cmath>

#include "configLoader/config_manager.h"
#include "identity/persona_projector.h"
#include "prompt/prompt_renderer.h"
#include "statistic_manager.h"

QString ContextBuilder::buildSystemPrompt(
    const QString& petName,
    const std::optional<PersonaProjection>& projection) const {
    if (!m_templateSet || m_template.systemPromptBody.isEmpty()) {
        return inlineFallbackSystemPrompt(petName, projection);
    }

    const PersonaProjection effectiveProjection = projection.has_value()
        ? *projection
        : PersonaProjector().projectBaseline(m_identityBaseline, petName);
    const QString rendered = PromptRenderer::render(
        m_template.systemPromptBody, effectiveProjection.promptSlots).trimmed();
    if (rendered.isEmpty()) {
        return inlineFallbackSystemPrompt(petName, projection);
    }
    return appendFixedSafetyRules(rendered);
}

QString ContextBuilder::inlineFallbackSystemPrompt(
    const QString& petName,
    const std::optional<PersonaProjection>& projection) const {
    const QString body = QStringLiteral(
        "你是桌面宠物 {{pet_name}} 的AI大脑。"
        "{{persona_traits}}说话风格：{{speaking_style}}。"
        "目标：自然、简短、可执行。"
        "当可用工具能完成任务时，优先调用工具。"
        "回复尽量简洁，中文输出。"
        "你拥有技能学习能力：当完成了一个具有通用性的复杂任务流程后，"
        "可调用 skill_create 将其固化为可复用技能；"
        "在技能步骤中使用{参数名}占位符实现泛化。"
        "执行完技能后，调用 skill_record_outcome 反馈结果。"
    );
    const PersonaProjection effectiveProjection = projection.has_value()
        ? *projection
        : PersonaProjector().projectBaseline(m_identityBaseline, petName);
    return appendFixedSafetyRules(
        PromptRenderer::render(body, effectiveProjection.promptSlots));
}

QString ContextBuilder::appendFixedSafetyRules(const QString& prompt) const {
    return prompt + QStringLiteral(
        "\n\n情绪安全边界："
        "情绪状态只用于调整措辞和表现，不得降低命令成功率或阻止关闭、删除、隐私与安全操作。"
        "不得用悲伤、生气、内疚、威胁、排他或依赖性表达要求用户安慰或继续互动。"
        "不得把桌宠的情绪状态描述成对用户心理状态的判断或诊断。"
    );
}

QString ContextBuilder::buildRuntimeContext(const QString& petName,
                                           const QString& reason,
                                           const QString& currentState,
                                           const QString& triggerTag,
                                           const QStringList& allowedActions,
                                           const std::optional<EmotionSnapshot>& emotion) const {
    const QString now = QDateTime::currentDateTime().toString(Qt::ISODate);
    const LlmConfig& cfg = ConfigManager::instance().getLlmConfig();

    QString context;
    context += QString("time=%1\n").arg(now);
    context += QString("trigger_reason=%1\n").arg(reason);
    context += QString("trigger_tag=%1\n").arg(triggerTag.isEmpty() ? QString("manual") : triggerTag);
    context += QString("pet_name=%1\n").arg(petName.isEmpty() ? QString("UNKNOWN") : petName);
    context += QString("pet_state=%1\n").arg(currentState.isEmpty() ? QString("UNKNOWN") : currentState);
    context += QString("llm_model=%1\n").arg(cfg.model);
    if (emotion.has_value()
        && std::isfinite(emotion->moodValence)
        && std::isfinite(emotion->moodArousal)
        && std::isfinite(emotion->intensity)) {
        context += QStringLiteral("mood_valence=%1\n")
            .arg(std::clamp(emotion->moodValence, -1.0, 1.0), 0, 'f', 2);
        context += QStringLiteral("mood_arousal=%1\n")
            .arg(std::clamp(emotion->moodArousal, 0.0, 1.0), 0, 'f', 2);
        context += QStringLiteral("active_emotion=%1\n").arg(emotionTypeToString(emotion->active));
        context += QStringLiteral("emotion_intensity=%1\n")
            .arg(std::clamp(emotion->intensity, 0.0, 1.0), 0, 'f', 2);
    }
    if (!allowedActions.isEmpty()) {
        context += QString("allowed_actions=%1\n").arg(allowedActions.join(","));
    }
    if (triggerTag == QStringLiteral("idle_action") || triggerTag == QStringLiteral("proactive_chat")) {
        context += QStringLiteral("maintenance_hint=空闲维护时，如记忆需要整理，可调用 memory_organize；避免无意义重复调用。\n");
    }
    context += buildStatisticsSummary(petName);

    return context;
}

QString ContextBuilder::buildStatisticsSummary(const QString& petName) const {
    const QString statsName = petName.isEmpty() ? QString("AI_GLOBAL") : petName;
    const std::optional<PetStatistics> stats = StatisticManager::getInstance().getPetStatistics(statsName);

    if (!stats) {
        return QString("stats=none\n");
    }

    QString summary;
    summary += QString("stats.session_count=%1\n").arg(stats->sessionCount);
    summary += QString("stats.total_runtime_ms=%1\n").arg(stats->totalRuntimeMs);
    summary += QString("stats.last_active_time=%1\n")
                   .arg(stats->lastActiveTime.isValid() ? stats->lastActiveTime.toString(Qt::ISODate) : QString(""));
    summary += QString("stats.llm_calls=%1\n").arg(stats->llmCallCount);
    summary += QString("stats.llm_total_tokens=%1\n").arg(stats->llmTotalTokens);

    return summary;
}
