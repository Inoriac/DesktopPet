#include "context_assembler.h"

#include <utility>

namespace {

QList<ContextPartition> allowedPartitions(ModelRole role) {
    switch (role) {
    case ModelRole::Dialogue:
        return {ContextPartition::CurrentInput, ContextPartition::Persona,
                ContextPartition::RelevantMemory, ContextPartition::SkillSummary};
    case ModelRole::FastExtract:
        return {ContextPartition::CurrentInput};
    case ModelRole::Consolidation:
        return {ContextPartition::EvidenceWindow,
                ContextPartition::RelevantMemory};
    case ModelRole::Diary:
        return {ContextPartition::DiaryProjection};
    case ModelRole::Vision:
        return {ContextPartition::VisionInput};
    }
    return {};
}

} // namespace

Result<QList<ChatMessage>, DomainError> ContextAssembler::assemble(
    ModelRole role, const ContextRequest& request) const {
    const QList<ContextPartition> allowed = allowedPartitions(role);
    for (ContextPartition partition : request.requestedPartitions) {
        if (!allowed.contains(partition)) {
            return Result<QList<ChatMessage>, DomainError>::failure(
                domainError(QStringLiteral("CONTEXT_SCOPE_DENIED"),
                            QStringLiteral("Requested context partition is forbidden for this role")));
        }
    }

    QList<ChatMessage> messages;
    int remaining = qMax(0, request.queryBudgetChars);
    for (ContextPartition requested : request.requestedPartitions) {
        for (const ContextProjection& projection : request.projections) {
            if (projection.partition != requested) continue;
            for (const ChatMessage& source : projection.messages) {
                if (remaining <= 0) return Result<QList<ChatMessage>, DomainError>::success(messages);
                ChatMessage bounded = source;
                if (bounded.content.size() > remaining) {
                    bounded.content = bounded.content.left(remaining);
                }
                remaining -= bounded.content.size();
                messages.append(std::move(bounded));
            }
        }
    }
    return Result<QList<ChatMessage>, DomainError>::success(messages);
}
