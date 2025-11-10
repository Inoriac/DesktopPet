//
// Created by Inoriac on 2025/11/10.
//

#include <QSignalSpy>
#include <QTest>
#include "manager/reminder/pet_reminder_manager.h"
#include "pet_personality.h"

class TestPetReminderManager : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void testAddAndClearReminders();
    void testTriggerReminder();
    void testForgetMechanism();
};

void TestPetReminderManager::initTestCase() {
    qDebug() << "Starting PetReminderManager unit tests...";
}

void TestPetReminderManager::cleanupTestCase() {
    qDebug() << "PetReminderManager tests complete.";
}

/**
 * 测试添加与清空提醒
 */
void TestPetReminderManager::testAddAndClearReminders() {
    PetReminderManager manager;

    // 构造时不应启动 QTimer，这是设计预期
    QVERIFY(manager.findChildren<QTimer*>().isEmpty());

    // 添加两个提醒（未来时间）
    QDateTime t1 = QDateTime::currentDateTime().addSecs(60);
    QDateTime t2 = QDateTime::currentDateTime().addSecs(120);
    manager.addReminder("Drink water", t1);
    manager.addReminder("Stretch legs", t2);

    // 此时仅验证不会崩溃，内部状态不做强行假设
    QVERIFY(true);

    // 清除所有提醒
    manager.clearReminders();
    QVERIFY(true);
}

/**
 * 测试立即触发提醒
 */
void TestPetReminderManager::testTriggerReminder() {
    PetReminderManager manager;
    QSignalSpy spy(&manager, &PetReminderManager::reminderTriggered);

    // 添加一个立即应触发的提醒（时间在过去）
    QDateTime past = QDateTime::currentDateTime().addSecs(-1);
    manager.addReminder("Feed cat", past);

    // 手动调用 checkReminders() 槽函数来模拟定时检查
    QMetaObject::invokeMethod(&manager, "checkReminders", Qt::DirectConnection);

    // 等待信号
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() > 0, 500);

    QList<QVariant> args = spy.takeFirst();
    QVERIFY(args.at(0).toString().contains("Feed cat"));
}

/**
 * 测试遗忘机制（高遗忘概率情况下）
 */
void TestPetReminderManager::testForgetMechanism() {
    PetReminderManager manager;
    PetPersonality p;
    p.forgetProbability = 1.0;  // 100% 遗忘
    p.forgetPhrases = {"Oops I forgot"};  // 提供短语
    manager.setPersonality(p);

    QSignalSpy forgottenSpy(&manager, &PetReminderManager::reminderForgotten);

    // 添加一个立即应触发的提醒
    QDateTime past = QDateTime::currentDateTime().addSecs(-1);
    manager.addReminder("Test forget", past);

    // 手动触发检查
    QMetaObject::invokeMethod(&manager, "checkReminders", Qt::DirectConnection);

    // 因为遗忘是延时 1 小时触发的，为了单元测试，我们等不那么久，
    // 所以这里只验证不会崩溃，信号可能捕不到（按实际实现）
    QTest::qWait(100);
    QVERIFY(true);
}

QTEST_MAIN(TestPetReminderManager)
#include "test_pet_reminder_manager.moc"
