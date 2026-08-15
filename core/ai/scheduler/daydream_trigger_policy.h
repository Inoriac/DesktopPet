#ifndef DESKTOP_PET_DAYDREAM_TRIGGER_POLICY_H
#define DESKTOP_PET_DAYDREAM_TRIGGER_POLICY_H

#include <QtGlobal>

// Daydream 空闲触发的复合判定策略（daydream.md 第三节）。纯逻辑、无 Qt 事件依赖，
// 便于在 memory_strategy_tests 单测各边界（AIBrain 不进测试 target）。
//
// 节流常量硬编码（daydream.md 第十节待确认表的推荐默认），配置化后置。
//   N1_IDLE_SEC    触发空闲阈值 5min
//   N2_MS_TO_DUE   距最近待办 >10min 才触发（防与待办抢资源）
//   MIN_GAP_MS     距上次 Daydream ≥15min
//   BACKOFF_MS     被打断后退避 ≥10min（防抖动）
//   HOURLY_CAP     每小时 ≤3 次
//   TICK_MS        监测 tick 30s
class DaydreamTriggerPolicy {
public:
    static constexpr int N1_IDLE_SEC = 300;
    static constexpr qint64 N2_MS_TO_DUE = 600000;
    static constexpr qint64 MIN_GAP_MS = 900000;
    static constexpr qint64 BACKOFF_MS = 600000;
    static constexpr int HOURLY_CAP = 3;
    static constexpr int TICK_MS = 30000;

    // 复合判定：全条件满足才 true。
    //   idleSec        queryUserIdleSeconds() 结果（<0 视为平台不支持 → 不触发）
    //   busy           AIBrain::m_busy
    //   msToNextDue    AgentScheduler::msToNextDue()（<0 无待办 → 不阻塞）
    //   msSinceLast    距上次 Daydream 的毫秒（<0 视为从未触发）
    //   wasInterrupted 上次 session 是否被打断（需额外 BACKOFF_MS 退避）
    //   countThisHour  本小时已触发次数
    bool shouldTrigger(int idleSec,
                       bool busy,
                       qint64 msToNextDue,
                       qint64 msSinceLast,
                       bool wasInterrupted,
                       int countThisHour) const;

    // Running sessions only need the conditions that can invalidate in-flight
    // work. Gap and hourly cap are start-time admission controls.
    bool shouldContinue(int idleSec,
                        bool busy,
                        qint64 msToNextDue) const;

    // 下一跳 tick 间隔（固定 TICK_MS；msSinceLast 仅留作未来自适应扩展的钩子）。
    int nextTickMs(qint64 msSinceLast) const;

    // 距上次需满足的最小间隔（被打断时叠加 BACKOFF_MS）。供 AIBrain 判定/日志用。
    qint64 requiredGapMs(bool wasInterrupted) const;
};

#endif // DESKTOP_PET_DAYDREAM_TRIGGER_POLICY_H
