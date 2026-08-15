#include "daydream_trigger_policy.h"

bool DaydreamTriggerPolicy::shouldTrigger(int idleSec,
                                          bool busy,
                                          qint64 msToNextDue,
                                          qint64 msSinceLast,
                                          bool wasInterrupted,
                                          int countThisHour) const {
    // 平台不支持空闲检测（<0）→ 默认不触发（宁可不 daydream 也不误触发）。
    if (idleSec < 0) return false;
    // 系统级全局空闲未达 N1。
    if (idleSec < N1_IDLE_SEC) return false;
    // 有进行中 LLM 对话。
    if (busy) return false;
    // 待办即将到期（<0 无待办，不阻塞）。
    if (msToNextDue >= 0 && msToNextDue < N2_MS_TO_DUE) return false;
    // 每小时上限。
    if (countThisHour >= HOURLY_CAP) return false;
    // 距上次需 ≥ MIN_GAP_MS；被打断则额外叠加 BACKOFF_MS 退避。
    if (msSinceLast >= 0 && msSinceLast < requiredGapMs(wasInterrupted)) return false;
    return true;
}

int DaydreamTriggerPolicy::nextTickMs(qint64 /*msSinceLast*/) const {
    return TICK_MS;
}

qint64 DaydreamTriggerPolicy::requiredGapMs(bool wasInterrupted) const {
    return wasInterrupted ? (MIN_GAP_MS + BACKOFF_MS) : MIN_GAP_MS;
}

bool DaydreamTriggerPolicy::shouldContinue(int idleSec,
                                           bool busy,
                                           qint64 msToNextDue) const {
    if (idleSec < N1_IDLE_SEC) return false;
    if (busy) return false;
    return msToNextDue < 0 || msToNextDue >= N2_MS_TO_DUE;
}
