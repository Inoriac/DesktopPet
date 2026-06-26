//
// ContextBuilder implementation
//

#include "context_builder.h"

#include <QDateTime>

#include "configLoader/config_manager.h"
#include "statistic_manager.h"

QString ContextBuilder::buildSystemPrompt(const QString& petName) const {
    const QString safePetName = petName.isEmpty() ? QString("桌宠") : petName;

    return QString(
        "你是桌面宠物 %1 的AI大脑。"
        "目标：自然、简短、可执行。"
        "当可用工具能完成任务时，优先调用工具。"
        "回复尽量简洁，中文输出。"
        "你拥有技能学习能力：当完成了一个具有通用性的复杂任务流程后，"
        "可调用 skill_create 将其固化为可复用技能；"
        "在技能步骤中使用{参数名}占位符实现泛化。"
        "执行完技能后，调用 skill_record_outcome 反馈结果。"
    ).arg(safePetName);
}

QString ContextBuilder::buildRuntimeContext(const QString& petName,
                                           const QString& reason,
                                           const QString& currentState,
                                           const QString& triggerTag,
                                           const QStringList& allowedActions) const {
    const QString now = QDateTime::currentDateTime().toString(Qt::ISODate);
    const LlmConfig& cfg = ConfigManager::instance().getLlmConfig();

    QString context;
    context += QString("time=%1\n").arg(now);
    context += QString("trigger_reason=%1\n").arg(reason);
    context += QString("trigger_tag=%1\n").arg(triggerTag.isEmpty() ? QString("manual") : triggerTag);
    context += QString("pet_name=%1\n").arg(petName.isEmpty() ? QString("UNKNOWN") : petName);
    context += QString("pet_state=%1\n").arg(currentState.isEmpty() ? QString("UNKNOWN") : currentState);
    context += QString("llm_model=%1\n").arg(cfg.model);
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
    PetStatistics* stats = StatisticManager::getInstance().getPetStatistics(statsName);

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
