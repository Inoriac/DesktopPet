//
// Created by Inoriac on 2025/11/10.
//

#include "pet_reminder_manager.h"

PetReminderManager::PetReminderManager(QObject *parent)
    : QObject(parent)
{
    connect(&checkTimer, &QTimer::timeout, this, &PetReminderManager::checkReminders);
}

void PetReminderManager::setPersonality(const PetPersonality &p) {
    personality = p;
}

void PetReminderManager::addReminder(const QString &desc, const QDateTime &targetTime) {
    PetReminder reminder(targetTime, desc);

    // 应用 personality 偏差
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> distrib(-personality.randomVariance,
                                                personality.randomVariance);
    qint64 offset = distrib(gen);
    reminder.targetTime = targetTime.addSecs(offset);

    reminders.append(reminder);
}

void PetReminderManager::removeReminder(const QDateTime &targetTime) {
    reminders.erase(
        std:: remove_if(reminders.begin(), reminders.end(),
            [&](const PetReminder& r) { return r.targetTime == targetTime; }),
        reminders.end()
    );
}

void PetReminderManager::clearReminders() {
    reminders.clear();
}

void PetReminderManager::checkReminders() {
    if (reminders.isEmpty()) {
        checkTimer.stop();
        active = false;
        return;
    }

    const QDateTime now = QDateTime::currentDateTime();

    for (auto& reminder : reminders) {
        if (reminder.triggered || reminder.isExpired(now)) continue;

        if (now > reminder.targetTime) {
            reminder.triggered = true;

            if (shouldForget()) {
                reminder.forgotten = true;
                QTimer::singleShot(3600 * 1000, this, [this, msg = reminder.message]() {
                    if (!personality.forgetPhrases.isEmpty()) {
                        QString phrase = personality.forgetPhrases[
                            QRandomGenerator::global()->bounded(personality.forgetPhrases.size())
                        ];
                        emit reminderForgotten(phrase + "(" + msg + ")");
                    }
                });
            } else {
                // 正常提醒
                if (!personality.reminderPhrases.isEmpty()) {
                    QString phrase = personality.reminderPhrases[
                        QRandomGenerator::global()->bounded(personality.reminderPhrases.size())
                    ];
                    emit reminderTriggered(phrase + " (" + reminder.message + ")");
                } else {
                    emit reminderTriggered(reminder.message);
                }
            }
        }
    }
}

bool PetReminderManager::shouldForget() const {
    double p = personality.forgetProbability;
    return QRandomGenerator::global()->bounded(1.0) < p;
}
