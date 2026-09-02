#include "chat_preparation_executor.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>

#include "ai/context_builder.h"
#include "ai/identity/persona_projector.h"
#include "ai/identity/sqlite_identity_repository.h"
#include "ai/memory/memory_extractor.h"
#include "ai/memory/memory_policy.h"
#include "ai/memory/memory_retriever.h"
#include "ai/memory/memory_relation_graph.h"
#include "ai/memory/sqlite_memory_repository.h"
#include "ai/runtime/runtime_types.h"

namespace {

MemoryQuery memoryQueryFor(const ChatPreparationRequest& request) {
    MemoryQuery query;
    query.text = request.reason;
    query.limit = 8;
    query.includeSensitive = false;
    query.includeInactive = false;
    if (request.emotion.has_value()) {
        query.currentEmotion = request.emotion->active;
        query.currentEmotionIntensity = request.emotion->intensity;
    }
    if (request.triggerTag == QLatin1String("user_request")
        || request.triggerTag == QLatin1String("manual")) {
        query.preferredTypes = {
            MemoryType::Preference, MemoryType::Semantic,
            MemoryType::Procedural, MemoryType::TaskShadow,
            MemoryType::Core, MemoryType::Relationship,
            MemoryType::Episodic
        };
    } else if (request.triggerTag == QLatin1String("proactive_chat")) {
        query.preferredTypes = {
            MemoryType::Preference, MemoryType::Relationship, MemoryType::Core
        };
    } else {
        query.preferredTypes = {MemoryType::Preference, MemoryType::Core};
        query.limit = 4;
    }
    return query;
}

QStringList tokenize(QString text) {
    text = text.toLower().trimmed();
    text.replace(QRegularExpression(
                     QStringLiteral("[^a-z0-9_\\x{4e00}-\\x{9fa5}]+")),
                 QStringLiteral(" "));
    return text.split(QRegularExpression(QStringLiteral("\\s+")),
                      Qt::SkipEmptyParts);
}

QStringList formatMatchingSkills(const QList<SkillEntry>& skills,
                                 const QString& reason) {
    const QString reasonLower = reason.toLower();
    const QStringList tokens = tokenize(reason);
    QStringList formatted;
    for (const SkillEntry& skill : skills) {
        QString searchable = skill.name + QLatin1Char(' ') + skill.description
            + QLatin1Char(' ') + skill.abstractGoal + QLatin1Char(' ')
            + skill.domain + QLatin1Char(' ') + skill.tags.join(QLatin1Char(' '));
        searchable = searchable.toLower();
        bool matches = false;
        for (const QString& pattern : skill.triggerPatterns) {
            if (!pattern.trimmed().isEmpty()
                && reasonLower.contains(pattern.trimmed(), Qt::CaseInsensitive)) {
                matches = true;
                break;
            }
        }
        for (const QString& token : tokens) {
            if (token.size() >= 2 && searchable.contains(token)) {
                matches = true;
                break;
            }
        }
        if (!matches) continue;

        QStringList lines;
        lines.append(QStringLiteral("=== 可参考技能 %1: %2 ===")
                         .arg(formatted.size() + 1)
                         .arg(skill.name));
        if (!skill.description.isEmpty()) {
            lines.append(QStringLiteral("描述: %1").arg(skill.description));
        }
        if (!skill.abstractGoal.isEmpty()) {
            lines.append(QStringLiteral("目标: %1").arg(skill.abstractGoal));
        }
        for (int i = 0; i < skill.steps.size(); ++i) {
            lines.append(QStringLiteral("  %1. %2")
                             .arg(i + 1)
                             .arg(skill.steps.at(i).instruction));
        }
        formatted.append(lines.join(QLatin1Char('\n')));
        if (formatted.size() == 2) break;
    }
    return formatted;
}

QStringList forgetQueriesFor(const ChatPreparationRequest& request) {
    QStringList queries;
    const QList<MemoryCandidate> candidates = MemoryExtractor().extractFromUserInput(
        request.reason, request.triggerTag);
    for (const MemoryCandidate& candidate : candidates) {
        if (candidate.operation == MemoryCandidateOperation::Forget
            && !candidate.query.trimmed().isEmpty()) {
            queries.append(candidate.query);
        }
    }
    return queries;
}

bool matchesAnyForgetQuery(const MemoryEntry& entry,
                           const QStringList& queries) {
    return std::any_of(queries.cbegin(), queries.cend(),
                       [&entry](const QString& query) {
                           return MemoryPolicy::matchesForgetQuery(entry, query);
                       });
}

bool matchesAnyForgetQuery(const WorkingMemoryItem& item,
                           const QStringList& queries) {
    MemoryEntry entry;
    entry.key = item.id;
    entry.summary = item.summary;
    entry.content = item.content;
    entry.tags = item.tags;
    return matchesAnyForgetQuery(entry, queries);
}

} // namespace

class ChatPreparationExecutor::Worker final : public QObject {
public:
    Worker(ChatPreparationEnvironment environment,
           std::shared_ptr<std::atomic<quint64>> cancellationEpoch
#ifdef DESKTOP_PET_ENABLE_TEST_SEAMS
           , std::shared_ptr<std::atomic<int>> testPreparationDelayMs,
           std::function<void(const QString&, quintptr)> lifecycleProbe
#endif
           )
        : m_environment(std::move(environment))
        , m_cancellationEpoch(std::move(cancellationEpoch))
#ifdef DESKTOP_PET_ENABLE_TEST_SEAMS
        , m_testPreparationDelayMs(std::move(testPreparationDelayMs))
        , m_lifecycleProbe(std::move(lifecycleProbe))
#endif
    {}

    void initialize() {
        const QFileInfo memoryDatabase(m_environment.memoryDatabasePath);
        if (memoryDatabase.exists() && memoryDatabase.isFile()) {
            auto memory = std::make_unique<SQLiteMemoryRepository>();
            QString ignored;
            if (memory->open(m_environment.memoryDatabasePath, &ignored)) {
                m_memoryRepository = std::move(memory);
                notifyLifecycle(QStringLiteral("memory.open"));
            }
        }
    }

    void shutdown() {
        if (m_memoryRepository) {
            m_memoryRepository->close();
            notifyLifecycle(QStringLiteral("memory.close"));
            m_memoryRepository.reset();
        }
        closeIdentityRepository();
    }

    ChatPreparationResult prepare(const ChatPreparationRequest& request,
                                  quint64 expectedEpoch) {
        QElapsedTimer timer;
        timer.start();
        ChatPreparationResult result;
        result.requestId = request.requestId;
        result.generation = request.generation;
        result.sessionId = request.sessionId;

        if (isCancelled(expectedEpoch)) return result;
#ifdef DESKTOP_PET_ENABLE_TEST_SEAMS
        int remainingDelay = m_testPreparationDelayMs
            ? std::max(0, m_testPreparationDelayMs->load()) : 0;
        if (remainingDelay > 0) {
            notifyLifecycle(QStringLiteral("preparation.delay.started"));
        }
        while (remainingDelay > 0 && !isCancelled(expectedEpoch)) {
            const int slice = std::min(remainingDelay, 5);
            QThread::msleep(static_cast<unsigned long>(slice));
            remainingDelay -= slice;
        }
        if (isCancelled(expectedEpoch)) return result;
#endif

        if (request.requestId.trimmed().isEmpty()) {
            result.error = domainError(
                QStringLiteral("CHAT_PREPARATION_INVALID_REQUEST"),
                QStringLiteral("chat preparation request is missing its identity"));
            result.preparationDurationMs = timer.elapsed();
            return result;
        }

        std::optional<PersonaProjection> projection;
        if (!request.sessionId.trimmed().isEmpty()
            && !request.runtimeMetadata.profileId.trimmed().isEmpty()) {
            RuntimeSnapshot snapshot;
            snapshot.sessionId = request.sessionId;
            snapshot.profileId = request.runtimeMetadata.profileId;
            snapshot.subjectId = request.runtimeMetadata.subjectId.trimmed().isEmpty()
                ? QStringLiteral("owner") : request.runtimeMetadata.subjectId;
            snapshot.identityBaselineSchemaVersion =
                request.runtimeMetadata.identityBaselineSchemaVersion;
            snapshot.identityBaselineHash = request.runtimeMetadata.identityBaselineHash;
            snapshot.configHash = request.runtimeMetadata.configHash;
            snapshot.capturedAt = QDateTime::currentDateTimeUtc();
            const bool identityOpen = ensureIdentityRepository(
                request.runtimeMetadata.runtimeDatabasePath);
            if (identityOpen && !isCancelled(expectedEpoch)) {
                const auto personality = m_identityRepository->currentPersonality(
                    snapshot.profileId);
                const auto relationship = m_identityRepository->currentRelationship(
                    snapshot.profileId, snapshot.subjectId);
                const auto selfModel = m_identityRepository->currentSelfModel(
                    snapshot.profileId);
                const bool identityReadable = personality.isOk()
                    && relationship.isOk() && selfModel.isOk();
                if (identityReadable) {
                    if (personality.value().has_value()) {
                        snapshot.personalityVersion = personality.value()->version;
                    }
                    if (relationship.value().has_value()) {
                        snapshot.relationshipVersion = relationship.value()->version;
                    }
                    if (selfModel.value().has_value()) {
                        snapshot.selfModelVersion = selfModel.value()->versionId;
                    }
                    PersonaProjector projector(request.identityBaseline,
                                               request.personalityPolicy,
                                               m_identityRepository.get(), nullptr);
                    projection = projector.project(
                        snapshot, {request.petName, QDateTime::currentDateTimeUtc()});
                }
            }
            result.runtimeSnapshot = snapshot;
        }

        if (isCancelled(expectedEpoch)) return result;

        ContextBuilder contextBuilder;
        contextBuilder.setIdentityBaseline(request.identityBaseline);
        if (!request.promptTemplate.systemPromptBody.isEmpty()) {
            contextBuilder.setPromptTemplate(request.promptTemplate);
        }

        ChatMessage systemMessage;
        systemMessage.role = QStringLiteral("system");
        systemMessage.content = contextBuilder.buildSystemPrompt(request.petName, projection);
        result.messages.append(systemMessage);

        for (const ChatMessage& memoryMessage : request.conversationMemory) {
            const bool conversational = memoryMessage.role == QLatin1String("user")
                || memoryMessage.role == QLatin1String("assistant");
            if (conversational && memoryMessage.toolCallId.isEmpty()
                && memoryMessage.toolCalls.isEmpty()) {
                result.messages.append(memoryMessage);
            }
        }

        ChatMessage contextMessage;
        contextMessage.role = QStringLiteral("user");
        contextMessage.content = contextBuilder.buildRuntimeContext(
            request.petName, request.reason, QStringLiteral("Idle"), request.triggerTag,
            request.allowedActions, projection.has_value() ? std::nullopt : request.emotion);

        if (m_memoryRepository) {
            QList<MemoryEntry> entries = m_memoryRepository->loadAll();
            notifyLifecycle(QStringLiteral("memory.load"));
            QList<WorkingMemoryItem> workingMemory = request.workingMemory;
            const QStringList forgetQueries = forgetQueriesFor(request);
            if (!forgetQueries.isEmpty()) {
                entries.erase(std::remove_if(
                                  entries.begin(), entries.end(),
                                  [&forgetQueries](const MemoryEntry& entry) {
                                      return matchesAnyForgetQuery(entry, forgetQueries);
                                  }),
                              entries.end());
                workingMemory.erase(std::remove_if(
                                        workingMemory.begin(), workingMemory.end(),
                                        [&forgetQueries](const WorkingMemoryItem& item) {
                                            return matchesAnyForgetQuery(item, forgetQueries);
                                        }),
                                    workingMemory.end());
            }
            MemoryRelationGraph relationGraph;
            relationGraph.setConnectionName(m_memoryRepository->connectionName());
            const QList<MemoryRelation> relations = relationGraph.all();
            if (isCancelled(expectedEpoch)) return result;
            MemoryRetriever retriever;
            const QList<RetrievedMemory> memories = retriever.retrieve(
                entries, memoryQueryFor(request), workingMemory, relations);
            const QStringList hints = retriever.formatForContext(memories);
            if (!hints.isEmpty()) {
                contextMessage.content += QStringLiteral("\n相关记忆：\n")
                    + hints.join(QLatin1Char('\n'))
                    + QStringLiteral(
                        "\n约束：不要编造未保存的历史；查询提醒时优先调用 schedule_list 获取真实任务状态；"
                        "敏感记忆未经确认不得主动暴露。\n");
            }
            QSet<QString> uniqueIds;
            for (const RetrievedMemory& memory : memories) {
                if (!memory.entry.id.startsWith(QLatin1String("wm:"))
                    && !memory.fromGraphExpansion
                    && !uniqueIds.contains(memory.entry.id)) {
                    uniqueIds.insert(memory.entry.id);
                    result.reinforcementIds.append(memory.entry.id);
                }
            }
        }

        const QStringList skillHints = formatMatchingSkills(request.skills, request.reason);
        if (!skillHints.isEmpty()) {
            contextMessage.content += QStringLiteral(
                "\n已学习的相关技能（可参考但不必严格遵循，按实际情况灵活运用）：\n")
                + skillHints.join(QLatin1Char('\n')) + QLatin1Char('\n');
        }
        result.messages.append(contextMessage);
        result.preparationDurationMs = timer.elapsed();
        return result;
    }

private:
    bool isCancelled(quint64 expectedEpoch) const {
        return !m_cancellationEpoch
            || m_cancellationEpoch->load(std::memory_order_acquire) != expectedEpoch;
    }

    bool ensureIdentityRepository(const QString& databasePath) {
        if (databasePath.trimmed().isEmpty()) {
            closeIdentityRepository();
            return false;
        }
        if (m_identityRepository && m_identityDatabasePath == databasePath) return true;
        closeIdentityRepository();
        const QFileInfo database(databasePath);
        if (!database.exists() || !database.isFile()) return false;
        auto identity = std::make_unique<SqliteIdentityRepository>();
        if (!identity->open(databasePath).isOk()) return false;
        m_identityRepository = std::move(identity);
        m_identityDatabasePath = databasePath;
        notifyLifecycle(QStringLiteral("identity.open"));
        return true;
    }

    void closeIdentityRepository() {
        if (!m_identityRepository) return;
        m_identityRepository->close();
        notifyLifecycle(QStringLiteral("identity.close"));
        m_identityRepository.reset();
        m_identityDatabasePath.clear();
    }

    void notifyLifecycle(const QString& phase) {
#ifdef DESKTOP_PET_ENABLE_TEST_SEAMS
        if (m_lifecycleProbe) {
            m_lifecycleProbe(
                phase, reinterpret_cast<quintptr>(QThread::currentThreadId()));
        }
#else
        Q_UNUSED(phase);
#endif
    }

    ChatPreparationEnvironment m_environment;
    std::shared_ptr<std::atomic<quint64>> m_cancellationEpoch;
    std::unique_ptr<SqliteIdentityRepository> m_identityRepository;
    QString m_identityDatabasePath;
    std::unique_ptr<SQLiteMemoryRepository> m_memoryRepository;
#ifdef DESKTOP_PET_ENABLE_TEST_SEAMS
    std::shared_ptr<std::atomic<int>> m_testPreparationDelayMs;
    std::function<void(const QString&, quintptr)> m_lifecycleProbe;
#endif
};

ChatPreparationExecutor::ChatPreparationExecutor(QObject* parent)
    : QObject(parent)
    , m_cancellationEpoch(std::make_shared<std::atomic<quint64>>(0))
#ifdef DESKTOP_PET_ENABLE_TEST_SEAMS
    , m_testPreparationDelayMs(std::make_shared<std::atomic<int>>(0))
#endif
{
    connect(&m_thread, &QThread::finished,
            this, &ChatPreparationExecutor::handleWorkerThreadFinished);
}

ChatPreparationExecutor::~ChatPreparationExecutor() {
    shutdownAndWait();
}

Result<void, DomainError> ChatPreparationExecutor::start(
    const ChatPreparationEnvironment& environment) {
    if (environment.memoryDatabasePath.trimmed().isEmpty()) {
        return Result<void, DomainError>::failure(domainError(
            QStringLiteral("CHAT_PREPARATION_INVALID_ENVIRONMENT"),
            QStringLiteral("chat preparation database paths are invalid")));
    }
    if (m_stopping) {
        m_pendingRestartEnvironment = environment;
        m_accepting = true;
        return Result<void, DomainError>::success();
    }
    if (m_accepting) {
        return Result<void, DomainError>::success();
    }
    finalizeStoppedWorker();
    if (m_thread.isRunning()) {
        return Result<void, DomainError>::failure(domainError(
            QStringLiteral("CHAT_PREPARATION_STOPPING"),
            QStringLiteral("chat preparation worker is still stopping")));
    }
    m_memoryDatabasePath = environment.memoryDatabasePath;
    m_worker = new Worker(environment, m_cancellationEpoch
#ifdef DESKTOP_PET_ENABLE_TEST_SEAMS
                          , m_testPreparationDelayMs,
                          m_testResourceLifecycleProbe
#endif
                          );
    m_worker->moveToThread(&m_thread);
    m_thread.start();
    if (!m_thread.isRunning()) {
        delete m_worker;
        m_worker = nullptr;
        return Result<void, DomainError>::failure(domainError(
            QStringLiteral("CHAT_PREPARATION_START_FAILED"),
            QStringLiteral("chat preparation thread did not start")));
    }
    const bool initialized = QMetaObject::invokeMethod(
        m_worker, [worker = m_worker]() { worker->initialize(); },
        Qt::BlockingQueuedConnection);
    if (!initialized) {
        shutdownAndWait();
        return Result<void, DomainError>::failure(domainError(
            QStringLiteral("CHAT_PREPARATION_START_FAILED"),
            QStringLiteral("chat preparation worker initialization failed")));
    }
    m_accepting = true;
    return Result<void, DomainError>::success();
}

void ChatPreparationExecutor::submit(ChatPreparationRequest request) {
    if (m_stopping && m_accepting && m_pendingRestartEnvironment.has_value()) {
        // Restart handoff is intentionally bounded to the latest request.
        m_pendingRequest = std::move(request);
        return;
    }
    if (!m_accepting || !m_worker || !m_thread.isRunning()) {
        ChatPreparationResult result;
        result.requestId = request.requestId;
        result.generation = request.generation;
        result.sessionId = request.sessionId;
        result.error = domainError(QStringLiteral("CHAT_PREPARATION_UNAVAILABLE"),
                                   QStringLiteral("chat preparation worker is unavailable"));
        QMetaObject::invokeMethod(
            this, [this, result = std::move(result)]() mutable {
                emit prepared(std::move(result));
            }, Qt::QueuedConnection);
        return;
    }

    QPointer<ChatPreparationExecutor> guarded(this);
    Worker* const worker = m_worker;
    const quint64 expectedEpoch = m_cancellationEpoch->load(
        std::memory_order_acquire);
    const auto cancellationEpoch = m_cancellationEpoch;
    QMetaObject::invokeMethod(
        worker,
        [worker, guarded, request = std::move(request), expectedEpoch,
         cancellationEpoch]() mutable {
            if (cancellationEpoch->load(std::memory_order_acquire) != expectedEpoch) return;
            ChatPreparationResult result = worker->prepare(request, expectedEpoch);
            if (!guarded
                || cancellationEpoch->load(std::memory_order_acquire) != expectedEpoch) {
                return;
            }
            QMetaObject::invokeMethod(
                guarded.data(),
                [guarded, result = std::move(result), expectedEpoch,
                 cancellationEpoch]() mutable {
                    if (guarded
                        && cancellationEpoch->load(std::memory_order_acquire)
                            == expectedEpoch) {
                        emit guarded->prepared(std::move(result));
                    }
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void ChatPreparationExecutor::stop() {
    m_accepting = false;
    m_cancellationEpoch->fetch_add(1, std::memory_order_acq_rel);
    m_pendingRestartEnvironment.reset();
    m_pendingRequest.reset();
    if (!m_worker || m_stopping) return;

    if (!m_thread.isRunning()) {
        finalizeStoppedWorker();
        return;
    }

    m_stopping = true;
    Worker* const worker = m_worker;
    QThread* const ownerThread = thread();
    QThread* const workerThread = &m_thread;
    const bool queued = QMetaObject::invokeMethod(
        worker,
        [worker, ownerThread, workerThread]() {
            worker->shutdown();
            worker->moveToThread(ownerThread);
            workerThread->quit();
        },
        Qt::QueuedConnection);
    if (!queued) {
        m_stopping = false;
    }
}

void ChatPreparationExecutor::finalizeStoppedWorker() {
    if (!m_worker || m_thread.isRunning()) return;
    delete m_worker;
    m_worker = nullptr;
    m_stopping = false;
    m_memoryDatabasePath.clear();
}

void ChatPreparationExecutor::handleWorkerThreadFinished() {
    std::optional<ChatPreparationEnvironment> restartEnvironment;
    std::optional<ChatPreparationRequest> pendingRequest;
    if (m_accepting && m_pendingRestartEnvironment.has_value()) {
        restartEnvironment = std::move(m_pendingRestartEnvironment);
        pendingRequest = std::move(m_pendingRequest);
    }
    m_pendingRestartEnvironment.reset();
    m_pendingRequest.reset();
    finalizeStoppedWorker();

    if (!restartEnvironment.has_value()) return;

    m_accepting = false;
    const auto restarted = start(*restartEnvironment);
    if (!restarted.isOk()) {
        if (pendingRequest.has_value()) {
            submit(std::move(*pendingRequest));
        }
        return;
    }
    if (pendingRequest.has_value()) {
        submit(std::move(*pendingRequest));
    }
}

void ChatPreparationExecutor::shutdownAndWait() {
    stop();
    if (m_thread.isRunning()) {
        // Destruction drains the queued cooperative shutdown. Normal stop()
        // only invalidates results and schedules this work.
        m_thread.wait();
    }
    finalizeStoppedWorker();
}

#ifdef DESKTOP_PET_ENABLE_TEST_SEAMS
void ChatPreparationExecutor::setTestPreparationDelayMs(int delayMs) {
    m_testPreparationDelayMs->store(std::max(0, delayMs), std::memory_order_release);
}

void ChatPreparationExecutor::setTestResourceLifecycleProbe(
    std::function<void(const QString&, quintptr)> probe) {
    m_testResourceLifecycleProbe = std::move(probe);
}

bool ChatPreparationExecutor::isWorkerThreadRunningForTests() const {
    return m_thread.isRunning();
}
#endif
