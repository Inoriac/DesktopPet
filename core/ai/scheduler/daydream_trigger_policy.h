#ifndef DESKTOP_PET_DAYDREAM_TRIGGER_POLICY_H
#define DESKTOP_PET_DAYDREAM_TRIGGER_POLICY_H
#include <QtGlobal>
#include "ai_types.h"

class DaydreamTriggerPolicy {
public:
    static constexpr qint64 MIN_GAP_MS = 900000;
    static constexpr int HOURLY_CAP = 3;
    static constexpr int TICK_MS = 30000;
    DaydreamTriggerPolicy() = default;
    explicit DaydreamTriggerPolicy(const DaydreamConfig& config) { configure(config); }
    void configure(const DaydreamConfig& config);
    bool shouldTrigger(qint64 msSinceLast, int countThisHour, int pendingCount = 0) const;
    int nextTickMs(qint64 msSinceLast) const;
private:
    qint64 m_minGapMs = MIN_GAP_MS;
    int m_hourlyLimit = HOURLY_CAP;
    int m_tickMs = TICK_MS;
};
#endif
