#ifndef DESKTOP_PET_AGENT_BOOTSTRAP_H
#define DESKTOP_PET_AGENT_BOOTSTRAP_H

#include "ai/domain/domain_result.h"
#include "runtime_types.h"

class AgentRuntimeServices;

class AgentBootstrap {
public:
    static Result<RuntimeStartReport, DomainError> start(
        AgentRuntimeServices& services,
        const RuntimeStartRequest& request);
};

#endif // DESKTOP_PET_AGENT_BOOTSTRAP_H
