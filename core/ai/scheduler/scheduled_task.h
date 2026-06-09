//
// Agent 调度任务数据模型
//

#ifndef DESKTOP_PET_SCHEDULED_TASK_H
#define DESKTOP_PET_SCHEDULED_TASK_H

#include <QDateTime>
#include <QJsonObject>
#include <QString>

struct ScheduledTask {
    QString id;
    bool enabled = true;
    QString title;
    QString description;
    QString source = "user_request";
    int priority = 50;

    QString triggerType = "once_at"; // once_at / daily_at / interval
    QDateTime onceAt;
    QTime dailyAt;
    int intervalMs = 0;

    bool respectQuietHours = true;
    bool skipWhenUserBusy = false;
    int minGapMs = 0;
    bool allowLlm = false;
    bool allowNetwork = false;

    QString message;
    QString animationState;

    QDateTime createdAt;
    QDateTime updatedAt;
    QDateTime lastTriggeredAt;
    QDateTime nextTriggerAt;

    QJsonObject toJson() const;
    static ScheduledTask fromJson(const QJsonObject& obj);

    bool isValid(QString* errorMessage = nullptr) const;
    void refreshNextTrigger(const QDateTime& now = QDateTime::currentDateTime());
    bool shouldTrigger(const QDateTime& now = QDateTime::currentDateTime()) const;
};

#endif // DESKTOP_PET_SCHEDULED_TASK_H
