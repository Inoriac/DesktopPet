#pragma once
#include "capability_types.h"
#include "ai/skill/skill_types.h"
class LearnedSkillAdapter {
public:
    static QJsonArray discover(const QList<SkillEntry>& snapshot,const ContextSpec& context);
    static Result<LoadedCapability,DomainError> load(const QList<SkillEntry>& snapshot,const CapabilityRef& ref,const ContextSpec& context);
};
