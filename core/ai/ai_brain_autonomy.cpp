#include "ai_brain.h"
#include "activity/activity_runtime.h"
#include "activity/activity_selector.h"
#include "plugin/plugin_runtime.h"
#include "reflection/inner_thought_service.h"
#include "runtime/agent_runtime_services.h"
#include <QPointer>

QJsonObject AIBrain::autonomyStatus() const {
    return {{"enabled", m_autonomyConfig.enabled},
            {"ready", m_autonomyConfigured},
            {"state", m_autonomyState},
            {"error", m_autonomyError},
            {"isIdle", m_autonomyIdleDecision.isIdle},
            {"canDeliberate", m_autonomyIdleDecision.canDeliberate},
            {"reason", m_autonomyIdleDecision.reason},
            {"paused", m_autonomyState == "paused"}};
}
void AIBrain::publishAutonomyStatus(const QString &state,
                                    const QString &error) {
    m_autonomyState = state;
    m_autonomyError = error;
    emit autonomyStatusChanged(autonomyStatus());
}
void AIBrain::configureAutonomyConsumer() {
    const auto generation = ++m_autonomyConfigurationGeneration;
    m_autonomyConfigured = false;
    if (!m_activityRuntime) {
        publishAutonomyStatus("failure", "RUNTIME_UNAVAILABLE");
        return;
    }
    m_activityRuntime->setOpportunityConsumer({});
    QPointer<AIBrain> self(this);
    QPointer<ActivityRuntime> runtime(m_activityRuntime.get());
    publishAutonomyStatus("configuring");
    m_activityRuntime->configureOpportunitiesAsync(
        m_autonomyConfig, [self, runtime, generation](auto result) {
            if (!self || !runtime || self->m_activityRuntime.get() != runtime ||
                self->m_autonomyConfigurationGeneration != generation)
                return;
            if (!result) {
                self->publishAutonomyStatus("failure", result.error().code);
                return;
            }
            self->m_autonomyConfigured = true;
            if (self->m_autonomyConfig.enabled)
                runtime->setOpportunityConsumer(
                    [self, runtime](OpportunityTicket ticket) {
                        if (self && runtime &&
                            self->m_activityRuntime.get() == runtime)
                            self->beginActivitySelection(std::move(ticket));
                    });
            self->publishAutonomyStatus(
                self->m_autonomyConfig.enabled ? "ready" : "disabled");
            self->requestAutonomousOpportunity("configuration");
        });
}
void AIBrain::beginActivitySelection(OpportunityTicket ticket) {
    if (!m_activitySelector || !m_activityRuntime || !m_autonomyConfigured)
        return;
    QPointer<AIBrain> self(this);
    QPointer<ActivityRuntime> runtime(m_activityRuntime.get());
    const auto generation = m_autonomyConfigurationGeneration;
    auto current = [self, runtime, generation] {
        return self && runtime && self->m_activityRuntime.get() == runtime &&
               self->m_autonomyConfigurationGeneration == generation;
    };
    auto finish = [self, runtime, current,
                   ticket](QString outcome, QString reason,
                           QDateTime next = QDateTime{}) {
        if (!current())
            return;
        OpportunityFinish f;
        f.ticketId = ticket.ticketId;
        f.hostRevision = ticket.hostRevision;
        f.inputWatermark = ticket.inputWatermark;
        f.outcome = outcome;
        f.reason = reason;
        f.nextConsiderAt = next;
        f.clock = OpportunityStore::systemClock();
        runtime->finishOpportunityAsync(
            f, [self, current, outcome, reason](auto result) {
                if (!current())
                    return;
                self->publishAutonomyStatus(
                    result ? outcome : "failure",
                    result ? (outcome == "failure" ? reason : QString())
                           : result.error().code);
            });
    };
    publishAutonomyStatus("preparing");
    SelectionRequest request;
    request.ticket = ticket;
    request.context.profileId = ticket.profileId;
    request.context.allowInternalMemory = true;
    request.context.audienceProjectionRef = "internal_activity";
    request.context.allowOwnerMemory = m_autonomyConfig.memoryAccessOwner;
    request.context.authorizedConversationIds =
        m_autonomyConfig.memoryAccessConversationIds;
    request.internalMemoryScope = request.context;
    request.runtimeDatabasePath = runtime->databasePath();
    request.memoryDatabasePath = m_memoryStore.databasePath();
    auto prepare = [self, runtime, current,
                    finish](SelectionRequest request) mutable {
        if (!current())
            return;
        self->m_activitySelector->prepareAsync(
            std::move(request),
            [self, runtime, current, finish](auto result) mutable {
                if (!current())
                    return;
                if (!result) {
                    finish("failure", result.error().code);
                    return;
                }
                auto request = result.value();
                auto related = request.context;
                if (related.originKind == OriginKind::Internal)
                    for (const auto &cue : request.cues)
                        for (const auto &ref : cue.sourceRefs)
                            if (ref.startsWith("person:") &&
                                related.participantPersonIds.size() < 8 &&
                                !related.participantPersonIds.contains(
                                    ref.mid(7)))
                                related.participantPersonIds << ref.mid(7);
                request.identity =
                    self->m_runtimeServices
                        ? self->m_runtimeServices->taskIdentityContext(
                              self->m_petName, related)
                        : QJsonObject{};
                if (self->m_pluginRuntime) {
                    request.catalogRevision =
                        self->m_pluginRuntime->catalogRevision();
                    request.capabilities =
                        self->m_pluginRuntime->discoverCapabilities(
                            request.context, true);
                    while (request.capabilities.size() > 16)
                        request.capabilities.removeLast();
                    for (int i = 0; i < request.capabilities.size(); ++i) {
                        auto cap = request.capabilities[i].toObject();
                        cap["summary"] = cap["summary"].toString().left(384);
                        cap["name"] = cap["name"].toString().left(128);
                        request.capabilities[i] = cap;
                    }
                }
                self->publishAutonomyStatus("selecting");
                self->m_activitySelector->selectActivity(
                    request,
                    runtime->modelBudgetHooks({}, request.ticket.ticketId),
                    [self, runtime, current, finish,
                     request](auto proposal) mutable {
                        if (!current())
                            return;
                        if (!proposal) {
                            finish("failure", proposal.error().code);
                            return;
                        }
                        if (proposal.value().decision == "wait") {
                            finish("wait", proposal.value().reason,
                                   proposal.value().nextConsiderAt);
                            return;
                        }
                        SelectionAdoption adoption{request, proposal.value()};
                        // Definition loads are asynchronous; actual invocation
                        // remains the runner's responsibility.
                        auto apply = [self, runtime, current, finish,
                                      adoption] {
                            if (!current())
                                return;
                            runtime->applySelectionAsync(
                                adoption, [self, current, finish](auto result) {
                                    if (!current())
                                        return;
                                    if (!result) {
                                        finish("failure", result.error().code);
                                        return;
                                    }
                                    self->publishAutonomyStatus("running");
                                });
                        };
                        if (adoption.proposal.decision != "start") {
                            apply();
                            return;
                        }
                        auto refs = std::make_shared<QList<CapabilityRef>>(
                            adoption.proposal.intent.capabilityRefs);
                        auto next = std::make_shared<std::function<void()>>();
                        std::weak_ptr<std::function<void()>> weak = next;
                        *next = [self, current, finish, request, refs, apply,
                                 weak] {
                            if (!current())
                                return;
                            if (refs->isEmpty()) {
                                apply();
                                return;
                            }
                            if (!self->m_pluginRuntime) {
                                finish("failure", "DEPENDENCY_UNAVAILABLE");
                                return;
                            }
                            auto ref = refs->takeFirst();
                            ToolExecutionRequest load;
                            load.requestId =
                                request.ticket.ticketId + ":load:" + ref.key();
                            load.toolName = "load_capability";
                            load.context = request.context;
                            load.autonomous = true;
                            load.arguments = {{"ref", ref.toJson()}};
                            auto keep = weak.lock();
                            self->m_pluginRuntime->executeGateway(
                                load,
                                [self, current, finish, keep](auto outcome) {
                                    if (!self)
                                        return;
                                    QMetaObject::invokeMethod(self, [current,
                                                                     finish,
                                                                     keep,
                                                                     outcome] {
                                        if (!current())
                                            return;
                                        if (!outcome.result.success) {
                                            finish("failure",
                                                   outcome.result.errorMessage);
                                            return;
                                        }
                                        if (keep)
                                            (*keep)();
                                    });
                                });
                        };
                        (*next)();
                    });
            });
    };
    if (ticket.contextId.isEmpty() && m_runtimeServices &&
        m_runtimeServices->innerThoughtService()) {
        m_runtimeServices->innerThoughtService()->recentUnresolvedAsync(
            4, [current, prepare, request](auto thoughts) mutable {
                if (!current())
                    return;
                if (!thoughts)
                    request.unavailable << "private thoughts unavailable: " +
                                               thoughts.error().code;
                else
                    for (const auto &thought : thoughts.value())
                        request.cues
                            << SelectionCue{"thought:" + thought.entryId,
                                            "thought",
                                            thought.desire.left(384) + " " +
                                                thought.uncertainty.left(384),
                                            {"event:" + thought.sourceEventId}};
                prepare(std::move(request));
            });
    } else {
        if (ticket.contextId.isEmpty())
            request.unavailable << "private thoughts unavailable";
        prepare(std::move(request));
    }
}
