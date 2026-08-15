//
// Created by Inoriac on 2025/10/28.
// 统计管理器功能测试
//

#include <QTest>
#include <QCoreApplication>
#include <QSignalSpy>
#include <QFile>
#include <QDir>
#include <thread>
#include <vector>

#include "statistic_manager.h"
#include "statistic_types.h"


class TestStatisticManager : public QObject {
    Q_OBJECT

private slots:
    // 初始化测试
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // 基础功能测试
    void testSingleton();
    void testInitialize();
    void testPetStartStop();
    void testTouchInteraction();
    void testStopAndTouchWithoutStart();
    void testDataQuery();

    // 事件系统测试
    void testEventSlotRegistration();
    void testEventEmission();
    void testSignalEmission();

    // 持久化测试
    void testSaveLoad();
    void testClearStatistics();

    // 配置测试
    void testAutoSaveConfiguration();

    // 边界条件测试
    void testConcurrentAccess();

private:
    QString testFilePath = "/config/pet_info.json";
    StatisticManager *manager = nullptr;
};


void TestStatisticManager::initTestCase() {
    // 设置测试文件路径
    testFilePath = QDir::tempPath() + "/test_statistics.json";

    // 清理可能存在的测试文件
    if(QFile::exists(testFilePath)) {
        QFile::remove(testFilePath);
    }
}

void TestStatisticManager::cleanupTestCase() {
    // 清理测试文件
    if(QFile::exists(testFilePath)) {
        QFile::remove(testFilePath);
    }
}

void TestStatisticManager::init() {
    manager = &StatisticManager::getInstance();
    manager->initialize(testFilePath, 5);   // 5秒自动保存
}

void TestStatisticManager::cleanup()
{
    // 每个用例结束时清理统计数据，保持用例相互独立
    StatisticManager::getInstance().clearStatistics();
}


void TestStatisticManager::testSingleton() {
    StatisticManager& instance1 = StatisticManager::getInstance();
    StatisticManager& instance2 = StatisticManager::getInstance();

    QCOMPARE(&instance1, &instance2);
}

void TestStatisticManager::testInitialize() {
    QVERIFY(manager->isAutoSaveEnabled());
    QCOMPARE(manager->getAutoSaveInterval(), 5);
}

void TestStatisticManager::testPetStartStop() {
    const QString petName = "TestPet";

    // 测试启动
    manager->recordPetStart(petName);
    std::optional<PetStatistics> stats = manager->getPetStatistics(petName);

    QVERIFY(stats.has_value());
    QCOMPARE(stats->petName, petName);
    QCOMPARE(stats->sessionCount, 1);

    QTest::qWait(1000);

    manager->recordPetStop(petName);
    stats = manager->getPetStatistics(petName);

    // 验证运行时长
    QVERIFY(stats->totalRuntimeMs > 0);
    QVERIFY(stats->sessionRuntimeMs > 0);

    qDebug() << "Total runtime:" << stats->totalRuntimeMs << "ms";
    qDebug() << "Session runtime:" << stats->sessionRuntimeMs << "ms";
}

void TestStatisticManager::testTouchInteraction() {
    const QString petName = "TestPet";
    const QString areaName = "head";

    manager->recordPetStart(petName);

    // 记录触摸交互
    manager->recordTouchInteraction(petName, areaName);

    std::optional<PetStatistics> stats = manager->getPetStatistics(petName);
    QVERIFY(stats.has_value());
    QCOMPARE(stats->touchAreaCount[areaName], 1);

    // 多次触摸
    manager->recordTouchInteraction(petName, areaName);
    manager->recordTouchInteraction(petName, "hand");
    stats = manager->getPetStatistics(petName);

    QCOMPARE(stats->touchAreaCount["head"], 2);
    QCOMPARE(stats->touchAreaCount["hand"], 1);
}

void TestStatisticManager::testStopAndTouchWithoutStart() {
    const QString petName = "OutOfOrderPet";
    manager->recordPetStop(petName);
    manager->recordTouchInteraction(petName, "head");

    const std::optional<PetStatistics> stats = manager->getPetStatistics(petName);
    QVERIFY(stats.has_value());
    QCOMPARE(stats->touchAreaCount.value("head"), 1);
    QVERIFY(!stats->isRunning);
}

void TestStatisticManager::testDataQuery() {
    const QString petName = "TestPet";

    std::optional<PetStatistics> stats = manager->getPetStatistics("NonExistentPet");
    QVERIFY(!stats.has_value());

    manager->recordPetStart(petName);
    stats = manager->getPetStatistics(petName);

    QVERIFY(stats.has_value());
    QCOMPARE(stats->petName, petName);

    // 测试获取所有的统计数据
    QHash<QString, PetStatistics> allStats = manager->getAllPetStatistics();
    QCOMPARE(allStats.size(), 1);
    QVERIFY(allStats.contains(petName));
}

void TestStatisticManager::testEventSlotRegistration() {
    bool slotCalled = false;
    StatisticEvent receivedEvent(StatisticEventType::PET_START, "TestPet", {});

    // 注册事件槽
    manager->registerEventSlot(StatisticEventType::PET_START,
        [&slotCalled, &receivedEvent](const StatisticEvent& event) {
            slotCalled = true;
            receivedEvent = event;
        });

    manager->recordPetStart("TestPet");

    QVERIFY(slotCalled);
    QCOMPARE(receivedEvent.type, StatisticEventType::PET_START);
    QCOMPARE(receivedEvent.petName, QString("TestPet"));
}

void TestStatisticManager::testEventEmission() {
    QSignalSpy eventSpy(manager, &StatisticManager::statisticEventOccurred);
    QSignalSpy updateSpy(manager, &StatisticManager::statisticsUpdated);

    manager->recordPetStart("TestPet");

    // 验证信号被发送
    QCOMPARE(eventSpy.count(), 1);
    QCOMPARE(updateSpy.count(), 1);
}

void TestStatisticManager::testSignalEmission()
{
    StatisticManager& m = StatisticManager::getInstance();

    QSignalSpy eventSpy(&m, &StatisticManager::statisticEventOccurred);
    QSignalSpy updateSpy(&m, &StatisticManager::statisticsUpdated);

    m.initialize(QDir::tempPath() + "/stat_test.json", 0);
    m.recordPetStart("TestPet_Signal");

    // 等待信号（如是 QueuedConnection，可适当等一下）
    QTest::qWait(10);

    QVERIFY(eventSpy.count() >= 1);
    QVERIFY(updateSpy.count() >= 1);
}

void TestStatisticManager::testSaveLoad() {
    const QString petName = "PersistedPet";
    manager->recordPetStart(petName);
    manager->recordTouchInteraction(petName, "head");
    manager->recordPetStop(petName);
    manager->saveStatistics();
    QVERIFY(QFile::exists(testFilePath));

    manager->recordTouchInteraction(petName, "not_persisted");
    manager->loadStatistics();
    const std::optional<PetStatistics> stats = manager->getPetStatistics(petName);
    QVERIFY(stats.has_value());
    QCOMPARE(stats->touchAreaCount.value("head"), 1);
    QCOMPARE(stats->touchAreaCount.value("not_persisted"), 0);
}

void TestStatisticManager::testClearStatistics()
{
    const QString petName1 = "Pet1";
    const QString petName2 = "Pet2";

    // 创建多个桌宠数据
    manager->recordPetStart(petName1);
    manager->recordPetStart(petName2);

    QCOMPARE(manager->getAllPetStatistics().size(), 2);

    // 清除特定桌宠
    manager->clearStatistics(petName1);
    QCOMPARE(manager->getAllPetStatistics().size(), 1);
    QVERIFY(!manager->getPetStatistics(petName1).has_value());
    QVERIFY(manager->getPetStatistics(petName2).has_value());

    // 清除所有数据
    manager->clearStatistics();
    QCOMPARE(manager->getAllPetStatistics().size(), 0);
}

void TestStatisticManager::testAutoSaveConfiguration()
{
    // 测试禁用自动保存
    manager->setAutoSaveEnabled(false);
    QVERIFY(!manager->isAutoSaveEnabled());

    // 测试修改保存间隔
    manager->setAutoSaveInterval(10);
    QCOMPARE(manager->getAutoSaveInterval(), 10);

    // 重新启用自动保存
    manager->setAutoSaveEnabled(true);
    QVERIFY(manager->isAutoSaveEnabled());
}

void TestStatisticManager::testConcurrentAccess()
{
    const QString petName = "ConcurrentTestPet";

    constexpr int threadCount = 8;
    constexpr int callsPerThread = 100;
    std::vector<std::thread> workers;
    for (int i = 0; i < threadCount; ++i) {
        workers.emplace_back([this, petName]() {
            LlmUsage usage;
            usage.totalTokens = 1;
            for (int call = 0; call < callsPerThread; ++call) {
                manager->recordLlmUsage(petName, usage);
                (void)manager->getPetStatistics(petName);
            }
        });
    }
    for (std::thread& worker : workers) worker.join();

    const std::optional<PetStatistics> stats = manager->getPetStatistics(petName);
    QVERIFY(stats.has_value());
    QCOMPARE(stats->llmCallCount, static_cast<qint64>(threadCount * callsPerThread));
    QCOMPARE(stats->llmTotalTokens, static_cast<qint64>(threadCount * callsPerThread));
}

QTEST_MAIN(TestStatisticManager)
#include "test_statistic_manager.moc"
