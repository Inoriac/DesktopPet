//
// Created by Inoriac on 2025/11/10.
//

#ifndef PET_REMINDER_H
#define PET_REMINDER_H

#include <QString>
#include <QDateTime>

struct PetReminder {
    QDateTime targetTime;   // 目标提醒时间
    QString message;        // 提醒内容
    bool triggered = false; // 是否已触发
    bool forgotten = false; // 是否遗忘

    PetReminder() = default;
    PetReminder(const QDateTime& time, const QString &msg)
        : targetTime(time), message(msg) {}

    // 判断该提醒是否已过期
    bool isExpired(const QDateTime& now) const {
        return now > targetTime.addSecs(3600);
    }
};


#endif // PET_REMINDER_H