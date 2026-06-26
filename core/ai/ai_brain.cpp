//
// AIBrain implementation
//

#include "ai_brain.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QCoreApplication>
#include <QDir>
#include <QUuid>

#include "configLoader/config_manager.h"

AIBrain::AIBrain(QObject* parent)
    : QObject(parent) {
    setupTriggerTimers();
    m_memoryStore.load();
    m_skillStore.load();
}

void AIBrain::setPetName(const QString& petName) {
    m_petName = petName;
}

void AIBrain::setToolRegistry(ToolRegistry* registry) {
    m_toolRegistry = registry;
    m_toolRuntime.setToolRegistry(registry);
}

void AIBrain::setEnabled(bool enabled) {
    m_enabled = enabled;
    if (!m_enabled) {
        stop();
    }
}

void AIBrain::setThinkIntervalMs(int ms) {
    if (ms < 1000) {
        ms = 1000;
    }
    m_idleTriggerTimer.setInterval(ms);
}

void AIBrain::start() {
    if (!m_enabled || m_running) {
        return;
    }

    m_running = true;
    scheduleTrigger("idle_action");
    scheduleTrigger("emotion");
    scheduleTrigger("proactive_chat");
}

void AIBrain::stop() {
    m_running = false;
    m_idleTriggerTimer.stop();
    m_emotionTriggerTimer.stop();
    m_chatTriggerTimer.stop();
    m_idleRetryScheduled = false;
}

void AIBrain::triggerThink(const QString& reason,
                           const QString& triggerTag) {
    if (!m_enabled || m_busy) {
        return;
    }

    processUserMemoryWrite(reason, triggerTag);

    if (shouldUseLocalRouter(triggerTag) && tryHandleRoutedIntent(reason, triggerTag)) {
        return;
    }

    QList<ChatMessage> base = buildBaseMessages(reason, triggerTag);
    if (base.isEmpty()) {
        return;
    }

    emit thinkingStarted(reason);
    m_busy = true;
    thinkInternal(reason, triggerTag, 0, base);
}

void AIBrain::onUserInteraction(const QString& eventName, const QString& detail) {
    const QString reason = detail.isEmpty()
                             ? QString("user_event:%1").arg(eventName)
                             : QString("user_event:%1:%2").arg(eventName, detail);
    triggerThink(reason, "touch_event");
}

void AIBrain::clearMemory() {
    m_memory.clear();
    m_memoryStore.clear();
    m_memoryStore.save();
}

bool AIBrain::shouldUseLocalRouter(const QString& triggerTag) const {
    return triggerTag == "manual" || triggerTag == "user_request";
}

void AIBrain::processUserMemoryWrite(const QString& input,
                                     const QString& triggerTag) {
    if (!shouldUseLocalRouter(triggerTag)) {
        return;
    }

    const QList<MemoryCandidate> candidates = m_memoryExtractor.extractFromUserInput(input, triggerTag);
    if (candidates.isEmpty()) {
        return;
    }

    m_memoryPolicy.applyCandidates(candidates, &m_memoryStore);
}

