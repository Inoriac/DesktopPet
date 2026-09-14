#ifndef DESKTOP_PET_CHAT_PREPARATION_TYPES_H
#define DESKTOP_PET_CHAT_PREPARATION_TYPES_H

#include <QList>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <optional>
#include <functional>
#include <memory>
#include "ai/memory/embedding_provider.h"

#include "ai/domain/domain_result.h"
#include "ai/identity/identity_baseline.h"
#include "ai/identity/identity_types.h"
#include "ai/memory/working_memory_cache.h"
#include "ai/prompt/prompt_template_types.h"
#include "ai/runtime/runtime_types.h"
#include "ai/skill/skill_types.h"
#include "ai_types.h"
#include "emotion/emotion_types.h"

struct ChatPreparationEnvironment {
    QString profileId;
    QString runtimeDatabasePath;
    QString memoryDatabasePath;
    QString embeddingAssetsDirectory;
#ifdef DESKTOP_PET_ENABLE_TEST_SEAMS
    // Called on the worker thread, never on the submitting thread.
    std::function<std::unique_ptr<EmbeddingProvider>()> embeddingProviderFactory;
#endif
    IdentityBaseline identityBaseline = IdentityBaseline::defaults();
    PersonalityPolicy personalityPolicy;
    PromptTemplate promptTemplate;
};

struct ChatPreparationRequest {
    QString requestId;
    quint64 generation = 0;
    QString sessionId;
    QString reason;
    QString triggerTag;
    QString petName;
    QStringList allowedActions;
    QList<ChatMessage> conversationMemory;
    QList<WorkingMemoryItem> workingMemory;
    QList<SkillEntry> skills;
    std::optional<EmotionSnapshot> emotion;
    ChatPreparationRuntimeMetadata runtimeMetadata;
    IdentityBaseline identityBaseline = IdentityBaseline::defaults();
    PersonalityPolicy personalityPolicy;
    PromptTemplate promptTemplate;
};

struct ChatPreparationResult {
    QString requestId;
    quint64 generation = 0;
    QString sessionId;
    QList<ChatMessage> messages;
    QStringList reinforcementIds;
    qint64 preparationDurationMs = 0;
    std::optional<RuntimeSnapshot> runtimeSnapshot;
    DomainError error;
};

#endif // DESKTOP_PET_CHAT_PREPARATION_TYPES_H
