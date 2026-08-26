#ifndef DESKTOP_PET_CONTEXT_ASSEMBLER_H
#define DESKTOP_PET_CONTEXT_ASSEMBLER_H

#include "ai/domain/domain_result.h"
#include "ai_types.h"

class ContextAssembler {
public:
    Result<QList<ChatMessage>, DomainError> assemble(
        ModelRole role, const ContextRequest& request) const;
};

#endif // DESKTOP_PET_CONTEXT_ASSEMBLER_H
