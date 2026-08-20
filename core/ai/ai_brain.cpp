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

#include <algorithm>
#include <cmath>
#include <utility>

#include "configLoader/config_manager.h"

AIBrain::AIBrain(QObject* parent)
    : QObject(parent) {
    m_daydreamConfig = ConfigManager::instance().getDaydreamConfig();
    m_daydreamPolicy.configure(m_daydreamConfig);
    setupTriggerTimers();
    m_memoryStore.load();
    m_skillStore.load();
}

void AIBrain::resolveToolConfirmation(const QString& requestId, bool approved) {
    const auto it = m_pendingToolConfirmations.find(requestId);
    if (it == m_pendingToolConfirmations.end()) {
        return;
    }
    std::function<void(bool)> continuation = std::move(it.value());
    m_pendingToolConfirmations.erase(it);
    continuation(approved);
}

void AIBrain::setPetName(const QString& petName) {
    m_petName = petName;
}

void AIBrain::setToolRegistry(ToolRegistry* registry) {
    if (m_toolRegistry != registry) {
        ++m_requestGeneration;
        m_pendingToolConfirmations.clear();
        m_busy = false;
    }
    m_toolRegistry = registry;
    m_toolRuntime.setToolRegistry(registry);
}

void AIBrain::setAgentScheduler(AgentScheduler* scheduler) {
    m_scheduler = scheduler;
}

void AIBrain::setEmotionSnapshotProvider(EmotionSnapshotProvider provider) {
    m_emotionSnapshotProvider = std::move(provider);
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
    scheduleTrigger("proactive_chat");
    if (m_daydreamConfig.enabled) {
        armDaydreamTimer();
    }
}

void AIBrain::stop() {
    if (m_daydreamRunning) {
        cancelDaydreamSession(QStringLiteral("AI brain stopped"));
    }
    ++m_requestGeneration;
    m_running = false;
    m_idleTriggerTimer.stop();
    m_chatTriggerTimer.stop();
    m_daydreamTimer.stop();
    m_idleRetryScheduled = false;
    m_toolRuntime.cancelPendingConfirmations(QStringLiteral("AI brain stopped"));
    m_pendingToolConfirmations.clear();
    m_busy = false;
}

void AIBrain::triggerThink(const QString& reason,
                           const QString& triggerTag) {
    if (m_daydreamRunning) {
        const bool userInitiated = triggerTag == QLatin1String("manual")
            || triggerTag == QLatin1String("user_request")
            || triggerTag == QLatin1String("touch_event");
        if (userInitiated) {
            cancelDaydreamSession(QStringLiteral("user interaction"));
        } else {
            if (m_running) scheduleTrigger(triggerTag);
            return;
        }
    }
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

    QList<MemoryCandidate> candidates = m_memoryExtractor.extractFromUserInput(input, triggerTag);
    if (!candidates.isEmpty()) {
        for (MemoryCandidate& candidate : candidates) {
            if (candidate.operation == MemoryCandidateOperation::Write) {
                annotateMemoryEntry(candidate.entry);
            }
        }
        // Explicit remember/forget requests keep their deterministic, immediate
        // semantics and must not also be duplicated into the Daydream inbox.
        m_memoryPolicy.applyCandidates(candidates, &m_memoryStore);
        return;
    }

    if (!m_daydreamConfig.enabled) {
        return;
    }

    MemoryEntry impression = m_memoryExtractor.extractDaydreamImpression(input, triggerTag);
    if (impression.content.isEmpty()) return;
    annotateMemoryEntry(impression);

    // Coalesce exact repeated self-disclosures so recurrence becomes a useful
    // consolidation signal instead of creating duplicate inbox rows.
    for (const MemoryEntry& existing : m_memoryStore.all()) {
        if (existing.status != MemoryStatus::Active
            || existing.partition != QLatin1String("hippocampus")
            || existing.key != impression.key) {
            continue;
        }
        MemoryEntry updated = existing;
        updated.mentionCount = qMax(1, existing.mentionCount) + 1;
        updated.updatedAt = QDateTime::currentDateTimeUtc();
        if (!updated.evidence.contains(impression.content)) {
            updated.evidence.append(impression.content);
        }
        m_memoryStore.updateEntryById(updated);
        return;
    }

    DaydreamConsolidator consolidator(m_memoryStore);
    if (consolidator.pendingCount() >= m_daydreamConfig.inboxLimit) {
        qWarning() << "[Daydream] inbox capacity reached; skipping new impression";
        return;
    }
    m_memoryStore.addEntry(impression);
}

std::optional<EmotionSnapshot> AIBrain::currentEmotionSnapshot() const {
    if (!m_emotionSnapshotProvider) {
        return std::nullopt;
    }
    const std::optional<EmotionSnapshot> snapshot = m_emotionSnapshotProvider();
    const int activeEmotion = snapshot.has_value()
        ? static_cast<int>(snapshot->active)
        : -1;
    if (!snapshot.has_value()
        || !snapshot->updatedAt.isValid()
        || activeEmotion < static_cast<int>(EmotionType::Neutral)
        || activeEmotion > static_cast<int>(EmotionType::Surprise)
        || !std::isfinite(snapshot->moodValence)
        || !std::isfinite(snapshot->moodArousal)
        || !std::isfinite(snapshot->intensity)
        || !std::isfinite(snapshot->confidence)
        || snapshot->moodValence < -1.0
        || snapshot->moodValence > 1.0
        || snapshot->moodArousal < 0.0
        || snapshot->moodArousal > 1.0
        || snapshot->intensity < 0.0
        || snapshot->intensity > 1.0
        || snapshot->confidence < 0.0
        || snapshot->confidence > 1.0) {
        return std::nullopt;
    }
    return snapshot;
}

void AIBrain::annotateMemoryEntry(MemoryEntry& entry) const {
    const std::optional<EmotionSnapshot> snapshot = currentEmotionSnapshot();
    if (!snapshot.has_value()
        || snapshot->active == EmotionType::Neutral
        || snapshot->intensity < 0.60
        || snapshot->confidence < 0.60) {
        return;
    }
    entry.emotion = snapshot->active;
    entry.emotionIntensity = std::clamp(snapshot->intensity, 0.0, 1.0);
    entry.emotionConfidence = std::clamp(snapshot->confidence, 0.0, 1.0);
}

