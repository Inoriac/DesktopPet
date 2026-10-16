#include "learned_skill_adapter.h"
QJsonArray LearnedSkillAdapter::discover(const QList<SkillEntry>& snapshot,const ContextSpec& context) {
    QJsonArray result; if (context.originKind!=OriginKind::LocalOwner) return result;
    for (const auto& skill:snapshot) result.append(QJsonObject{{"kind","skill"},{"pluginId","builtin.learned"},{"id",skill.id},
        {"name",skill.name},{"summary",skill.description},{"version",QString::number(skill.version)},{"definitionRef",skill.id},{"readiness","ready"}});
    return result;
}
Result<LoadedCapability,DomainError> LearnedSkillAdapter::load(const QList<SkillEntry>& snapshot,const CapabilityRef& ref,const ContextSpec& context) {
    if (context.originKind==OriginKind::LocalOwner && ref.pluginId=="builtin.learned" && ref.operationId.isEmpty())
        for (const auto& skill:snapshot) if (skill.id==ref.entryId && QString::number(skill.version)==ref.entryVersion) {
            LoadedCapability loaded; loaded.kind="skill"; loaded.ref=ref;
            loaded.instructions=skill.description+"\n"+skill.abstractGoal;
            for (const auto& step:skill.steps) loaded.instructions+="\n"+step.instruction+"\n"+step.condition+"\n"+step.toolHint;
            return Result<LoadedCapability,DomainError>::success(loaded);
        }
    return Result<LoadedCapability,DomainError>::failure(domainError("UNKNOWN_REFERENCE","Learned workflow is unavailable in this scope"));
}
