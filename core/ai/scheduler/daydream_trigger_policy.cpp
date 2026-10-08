#include "daydream_trigger_policy.h"
void DaydreamTriggerPolicy::configure(const DaydreamConfig& config) {
    m_minGapMs = config.minIntervalMs;
    m_hourlyLimit = config.hourlyLimit;
    m_tickMs = config.tickIntervalMs;
}
bool DaydreamTriggerPolicy::shouldTrigger(qint64 msSinceLast, int countThisHour, int pendingCount) const {
    const qint64 gap = pendingCount >= 64 ? qMin<qint64>(m_minGapMs, 60000) : m_minGapMs;
    return countThisHour < m_hourlyLimit && (msSinceLast < 0 || msSinceLast >= gap);
}
int DaydreamTriggerPolicy::nextTickMs(qint64) const { return m_tickMs; }
