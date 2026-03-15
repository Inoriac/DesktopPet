//
// Created by Inoriac on 2025/11/10.
//

#ifndef PET_REMINDER_MANAGER_H
#define PET_REMINDER_MANAGER_H

#include <QObject>
#include <QTimer>
#include <QList>
#include <QRandomGenerator>

#include "../../include/pet_reminder_types.h"
#include "pet_personality.h"

class PetReminderManager : public QObject {
    Q_OBJECT

public:
    explicit PetReminderManager(QObject* parent = nullptr);
    ~PetReminderManager() override = default;

    void setPersonality(const PetPersonality& p);
    void addReminder(const QString& desc, const QDateTime& targetTime);
    void removeReminder(const QDateTime& targetTime);
    void clearReminders();

signals:
    // 正常触发提醒
    void reminderTriggered(const QString& message);
    // 遗忘提醒的补偿反应
    void reminderForgotten(const QString& message);

private slots:
    // 定期检查提醒任务
    void checkReminders();

private:
    QList<PetReminder> reminders;
    PetPersonality personality;
    QTimer checkTimer;
    bool active = false;

    // 决定是否遗忘
    bool shouldForget() const;
};

#endif // PET_REMINDER_MANAGER_H