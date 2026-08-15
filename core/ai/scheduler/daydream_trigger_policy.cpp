#include "daydream_trigger_policy.h"

DaydreamTriggerPolicy::DaydreamTriggerPolicy(const DaydreamConfig& config) {
    configure(config);
}

void DaydreamTriggerPolicy::configure(const DaydreamConfig& config) {
    m_idleThresholdSec = config.idleThresholdSec;
    m_dueSoonThresholdMs = config.dueSoonThresholdMs;
    m_minGapMs = config.minIntervalMs;
    m_backoffMs = config.interruptionBackoffMs;
    m_hourlyLimit = config.hourlyLimit;
    m_tickMs = config.tickIntervalMs;
}

bool DaydreamTriggerPolicy::shouldTrigger(int idleSec,
                                          bool busy,
                                          qint64 msToNextDue,
                                          qint64 msSinceLast,
                                          bool wasInterrupted,
                                          int countThisHour) const {
    if (idleSec < 0 || idleSec < m_idleThresholdSec) return false;
    if (busy) return false;
    if (msToNextDue >= 0 && msToNextDue < m_dueSoonThresholdMs) return false;
    if (countThisHour >= m_hourlyLimit) return false;
    if (msSinceLast >= 0 && msSinceLast < requiredGapMs(wasInterrupted)) return false;
    return true;
}

int DaydreamTriggerPolicy::nextTickMs(qint64 /*msSinceLast*/) const {
    return m_tickMs;
}

qint64 DaydreamTriggerPolicy::requiredGapMs(bool wasInterrupted) const {
    return wasInterrupted ? (m_minGapMs + m_backoffMs) : m_minGapMs;
}

bool DaydreamTriggerPolicy::shouldContinue(int idleSec,
                                           bool busy,
                                           qint64 msToNextDue) const {
    if (idleSec < m_idleThresholdSec) return false;
    if (busy) return false;
    return msToNextDue < 0 || msToNextDue >= m_dueSoonThresholdMs;
}
