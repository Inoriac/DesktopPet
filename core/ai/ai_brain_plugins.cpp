#include "ai_brain.h"
#include "chat/conversation_service.h"
#include "plugin/plugin_runtime.h"
#include "runtime/agent_runtime_services.h"
#include <QDir>
#include <QFileInfo>

void AIBrain::retirePluginRuntime() {
    if (!m_pluginRuntime) return;
    auto* retired=m_pluginRuntime.release();
    // Old dispatches keep their immutable profile and receipt callback while draining.
    retired->disconnect(this);
    retired->retire();
}
void AIBrain::initializePluginRuntime() {
    if (!m_runtimeServices || !m_toolRegistry || m_chatPreparationRuntimeMetadata.runtimeDatabasePath.isEmpty()) return;
    if (m_pluginRuntime) { m_pluginRuntime->setLearnedSkills(m_skillStore.all()); return; }
    // A replaced profile's gateway closures have weak references to its retired runtime.
    for (const auto& name:{"discover_capabilities","load_capability","invoke_capability"}) m_toolRegistry->unregisterTool(name);
    m_pluginRuntime=std::make_unique<PluginRuntime>(m_chatPreparationRuntimeMetadata.profileId,
        QDir(QFileInfo(m_chatPreparationRuntimeMetadata.runtimeDatabasePath).absolutePath()).filePath("plugins.json"));
    m_pluginRuntime->setObservationDatabase(
        m_chatPreparationRuntimeMetadata.runtimeDatabasePath);
    connect(m_pluginRuntime.get(), &PluginRuntime::observationAccepted, this,
            [this,
             runtime = m_pluginRuntime.get()](const AcceptedMessage &message) {
              if (m_pluginRuntime.get() != runtime ||
                  message.context.profileId !=
                      m_chatPreparationRuntimeMetadata.profileId)
                return;
              if (message.duplicate || message.selfMessage ||
                  message.historicalImport)
                return;
              m_opportunityInputWatermarks[message.conversationId] =
                  message.messageId;
              ++m_interactionRevision;
              requestAutonomousOpportunity("important_reply",
                                           message.conversationId,
                                           {message.originId});
            });
    connect(m_pluginRuntime.get(), &PluginRuntime::configurationChanged, this,
            [this, runtime = m_pluginRuntime.get()] {
              if (m_pluginRuntime.get() != runtime)
                return;
              if (auto *service = m_runtimeServices
                                      ? m_runtimeServices->conversationService()
                                      : nullptr)
                for (const auto &binding :
                     m_pluginRuntime->conversationBindings())
                  service->registerBinding(binding);
              refreshTaskDependencies();
            });
    const auto initialized=m_pluginRuntime->initialize();
    if (!initialized) qWarning()<<"[PluginRuntime]"<<initialized.error().code;
    m_pluginRuntime->setLearnedSkills(m_skillStore.all());
    m_pluginRuntime->attach(m_toolRegistry);
    if (auto* service=m_runtimeServices->conversationService())
        for (const auto& binding:m_pluginRuntime->conversationBindings()) service->registerBinding(binding);
}
ContextSpec AIBrain::currentToolContext() const {
    if (m_activeDialogueResponse && m_activeDialogueResponse->context && !m_activeDialogueResponse->context->profileId.isEmpty()) return *m_activeDialogueResponse->context;
    return ContextSpec::localOwner(m_chatPreparationRuntimeMetadata.profileId);
}
