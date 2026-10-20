//
// Created by Inoriac on 2025/10/28.
// 统计管理器功能测试
//

#include <QTest>
#include <QCoreApplication>
#include <QSignalSpy>
#include <QFile>
#include <QDir>
#include <QTemporaryDir>
#include <QLockFile>
#include <QUuid>
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
    void testLlmCallOutcomePersistence();

    // 边界条件测试
    void testConcurrentAccess();
    void attemptRollupsConsistent();
    void missingVsReportedZeroUsage();
    void profileRestartRenameIsolation();
    void legacyImportWithoutFakeDetails();
    void runtimeCheckpointNotDoubleCounted();
    void initializationClearsPreviousData();
    void boundedRecentHistory();
    void persistenceFailureVisible();
    void lockedProfileUsesMemoryOnly();

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

void TestStatisticManager::testLlmCallOutcomePersistence()
{
    const QString petName = QStringLiteral("StatisticsPet");
    LlmUsage successUsage;
    successUsage.promptTokens = 10;
    successUsage.completionTokens = 5;
    successUsage.totalTokens = 15;
    LlmUsage failureUsage;
    failureUsage.promptTokens = 3;
    failureUsage.totalTokens = 3;

    manager->recordLlmCall(petName, true, successUsage);
    manager->recordLlmCall(petName, false, failureUsage);
    manager->saveStatistics();
    manager->recordLlmCall(petName, false, failureUsage);
    manager->loadStatistics();

    const std::optional<PetStatistics> stats =
        manager->getPetStatistics(petName);
    QVERIFY(stats.has_value());
    QCOMPARE(stats->llmCallCount, static_cast<qint64>(2));
    QCOMPARE(stats->llmSuccessCount, static_cast<qint64>(1));
    QCOMPARE(stats->llmFailureCount, static_cast<qint64>(1));
    QCOMPARE(stats->llmPromptTokens, static_cast<qint64>(13));
    QCOMPARE(stats->llmCompletionTokens, static_cast<qint64>(5));
    QCOMPARE(stats->llmTotalTokens, static_cast<qint64>(18));
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


namespace {
const QString profileA = QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
const QString profileB = QStringLiteral("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");

ModelAttemptReport attempt(const QString& id, const QDateTime& at) {
    ModelAttemptReport report;
    report.attemptId = id;
    report.profileId = profileA;
    report.petName = QStringLiteral("diary_fragment");
    report.role = QStringLiteral("diary");
    report.provider = QStringLiteral("test-provider");
    report.model = QStringLiteral("test-model");
    report.routeId = QStringLiteral("primary");
    report.success = true;
    report.durationMs = 25;
    report.completedAt = at;
    report.usage.promptTokens = 10;
    report.usage.completionTokens = 3;
    report.usage.totalTokens = 13;
    return report;
}

qint64 count(const QJsonObject& object, const QString& key) {
    return object.value(key).toInteger();
}
}

void TestStatisticManager::attemptRollupsConsistent() {
    QTemporaryDir dir;
    manager->initializeForProfile(dir.path(), profileA, "Pet", {}, false);
    const auto now = QDateTime::currentDateTimeUtc();
    auto success = attempt("success", now);
    manager->recordLlmAttempt(success);
    auto failure = attempt("failure", now);
    failure.success = false;
    failure.usage = {};
    manager->recordLlmAttempt(failure);
    auto cancelled = attempt("cancelled", now);
    cancelled.success = false;
    cancelled.cancelled = true;
    cancelled.usage = {};
    manager->recordLlmAttempt(cancelled);
    const auto view = manager->overview(now);
    const auto all = view["allTime"].toObject();
    QCOMPARE(count(all, "callCount"), 3);
    QCOMPARE(count(all, "successCount"), 1);
    QCOMPARE(count(all, "failureCount"), 1);
    QCOMPARE(count(all, "cancelledCount"), 1);
    QCOMPARE(count(all, "unknownUsageCount"), 2);
    QCOMPARE(count(all, "totalTokens"), 13);
    QCOMPARE(view["today"].toObject(), all);
    QCOMPARE(view["session"].toObject(), all);
    QCOMPARE(view["byModel"].toArray().size(), 1);
    QCOMPARE(count(view["byModel"].toArray().first().toObject(), "callCount"), 3);
    QCOMPARE(view["recentCalls"].toArray().size(), 3);
    const auto recent = view["recentCalls"].toArray().first().toObject();
    QCOMPARE(recent["result"].toString(), QString("cancelled"));
    QVERIFY(!recent.contains("prompt"));
    const auto compatibility = manager->getPetStatistics("Pet");
    QVERIFY(compatibility);
    QCOMPARE(compatibility->llmCallCount, 3);
    QCOMPARE(compatibility->llmTotalTokens, 13);
    QCOMPARE(manager->getAllPetStatistics().size(), 1);
    auto other = attempt("other-profile", now);
    other.profileId = profileB;
    manager->recordLlmAttempt(other);
    QCOMPARE(count(manager->overview(now)["allTime"].toObject(), "callCount"), 3);
    QVERIFY(!manager->persistenceWarning().isEmpty());
}

void TestStatisticManager::missingVsReportedZeroUsage() {
    QTemporaryDir dir;
    manager->initializeForProfile(dir.path(), profileA, "Pet", {}, false);
    const auto now = QDateTime::currentDateTimeUtc();
    auto missing = attempt("missing", now);
    missing.usage = {};
    manager->recordLlmAttempt(missing);
    auto zero = missing;
    zero.attemptId = "zero";
    zero.usage.reported = true;
    manager->recordLlmAttempt(zero);
    const auto view = manager->overview(now);
    QCOMPARE(count(view["allTime"].toObject(), "unknownUsageCount"), 1);
    QCOMPARE(count(view["allTime"].toObject(), "totalTokens"), 0);
    QVERIFY(view["recentCalls"].toArray().first().toObject()["usageReported"].toBool());
    QVERIFY(!view["recentCalls"].toArray().last().toObject()["usageReported"].toBool());
}

void TestStatisticManager::profileRestartRenameIsolation() {
    QTemporaryDir dir;
    manager->initializeForProfile(dir.path(), profileA, "OldName", {}, false);
    manager->recordLlmAttempt(attempt("first", QDateTime::currentDateTimeUtc()));
    manager->saveStatistics();
    manager->initializeForProfile(dir.path(), profileA, "NewName", {}, false);
    QCOMPARE(count(manager->overview()["allTime"].toObject(), "callCount"), 1);
    QCOMPARE(count(manager->overview()["session"].toObject(), "callCount"), 0);
    QVERIFY(manager->getPetStatistics("NewName"));
    QVERIFY(!manager->getPetStatistics("OldName"));
    manager->recordLlmCall("", true, LlmUsage{});
    QCOMPARE(manager->getPetStatistics("NewName")->llmCallCount, 2);
    manager->initializeForProfile(dir.path(), profileB, "NewName", {}, false);
    QCOMPARE(count(manager->overview()["allTime"].toObject(), "callCount"), 0);
}

void TestStatisticManager::legacyImportWithoutFakeDetails() {
    QTemporaryDir dir;
    const QString legacy = dir.filePath("legacy.json");
    manager->initialize(legacy, 0);
    LlmUsage usage;
    usage.totalTokens = 55;
    manager->recordLlmCall("Pet", true, usage);
    manager->saveStatistics();
    QFile old(legacy);
    QVERIFY(old.open(QIODevice::ReadOnly));
    const auto before = old.readAll();
    old.close();
    manager->initializeForProfile(dir.path(), profileA, "Pet", legacy, true);
    auto view = manager->overview();
    QVERIFY(view["legacyImported"].toBool());
    QCOMPARE(count(view["allTime"].toObject(), "totalTokens"), 55);
    QCOMPARE(count(view["today"].toObject(), "callCount"), 0);
    QVERIFY(view["byModel"].toArray().isEmpty());
    QVERIFY(view["recentCalls"].toArray().isEmpty());
    manager->saveStatistics();
    QVERIFY(old.open(QIODevice::ReadOnly));
    QCOMPARE(old.readAll(), before);
    manager->initializeForProfile(dir.path(), profileB, "Pet", legacy, false);
    QVERIFY(!manager->overview()["legacyImported"].toBool());
    QCOMPARE(count(manager->overview()["allTime"].toObject(), "callCount"), 0);
}

void TestStatisticManager::runtimeCheckpointNotDoubleCounted() {
    QTemporaryDir dir;
    manager->initializeForProfile(dir.path(), profileA, "Pet", {}, false);
    manager->recordPetStart("Pet");
    QTest::qWait(30);
    manager->saveStatistics();
    QTest::qWait(20);
    manager->saveStatistics();
    QFile saved(dir.filePath("profiles/" + profileA + "/statistics.json"));
    QVERIFY(saved.open(QIODevice::ReadOnly));
    const auto root = QJsonDocument::fromJson(saved.readAll()).object();
    const auto total = count(root["statistics"].toObject()["uptime"].toObject(), "totalMs");
    QVERIFY(total >= 50);
    manager->initializeForProfile(dir.path(), profileA, "Pet", {}, false);
    QCOMPARE(count(manager->overview()["uptime"].toObject(), "totalMs"), total);
    QCOMPARE(count(manager->overview()["uptime"].toObject(), "sessionMs"), 0);
    manager->recordPetStart("Pet");
    QTest::qWait(20);
    manager->recordPetStop("Pet");
    const auto stopped = manager->overview()["uptime"].toObject();
    QCOMPARE(count(stopped, "totalMs"), total + count(stopped, "sessionMs"));
    manager->saveStatistics();
    manager->loadStatistics();
    QCOMPARE(count(manager->overview()["uptime"].toObject(), "totalMs"), count(stopped, "totalMs"));
}

void TestStatisticManager::initializationClearsPreviousData() {
    QTemporaryDir dir;
    manager->recordLlmCall("stale", true, LlmUsage{});
    manager->initialize(dir.filePath("missing.json"), 0);
    QVERIFY(manager->getAllPetStatistics().isEmpty());
    manager->recordPetStart("fresh");
    QVERIFY(!manager->getPetStatistics("stale"));
}

void TestStatisticManager::boundedRecentHistory() {
    QTemporaryDir dir;
    manager->initializeForProfile(dir.path(), profileA, "Pet", {}, false);
    const auto now = QDateTime::currentDateTimeUtc();
    for (int i = 0; i < 220; ++i)
        manager->recordLlmAttempt(attempt(QString::number(i), now.addDays(-219 + i)));
    const auto view = manager->overview(now);
    QCOMPARE(count(view["allTime"].toObject(), "callCount"), 220);
    QCOMPARE(view["recentCalls"].toArray().size(), 200);
    QCOMPARE(view["recentCalls"].toArray().first().toObject()["attemptId"].toString(), QString("219"));
    QCOMPARE(view["daily"].toArray().size(), 90);
    QCOMPARE(count(view["today"].toObject(), "callCount"), 1);
}

void TestStatisticManager::persistenceFailureVisible() {
    QTemporaryDir dir;
    const QString folder = dir.filePath("profiles/" + profileA);
    QVERIFY(QDir().mkpath(folder));
    const QString path = folder + "/statistics.json";
    for (const QByteArray bad : {QByteArray("not json"), QByteArray(R"({"version":2,"profileId":"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa","statistics":null})")}) {
        QFile broken(path);
        QVERIFY(broken.open(QIODevice::WriteOnly | QIODevice::Truncate));
        broken.write(bad);
        broken.close();
        manager->initializeForProfile(dir.path(), profileA, "Pet", {}, false);
        QVERIFY(!manager->persistenceWarning().isEmpty());
        manager->recordLlmAttempt(attempt("memory-only", QDateTime::currentDateTimeUtc()));
        manager->saveStatistics();
        QCOMPARE(count(manager->overview()["allTime"].toObject(), "callCount"), 1);
        QVERIFY(broken.open(QIODevice::ReadOnly));
        QCOMPARE(broken.readAll(), bad);
        manager->initialize("", 0);
    }
    manager->initialize(dir.path(), 0);
    manager->recordLlmCall("Pet", true, {});
    manager->saveStatistics();
    QVERIFY(!manager->persistenceWarning().isEmpty());
}

void TestStatisticManager::lockedProfileUsesMemoryOnly() {
    QTemporaryDir dir;
    const QString folder = dir.filePath("profiles/" + profileA);
    QVERIFY(QDir().mkpath(folder));
    QLockFile lock(folder + "/statistics.json.lock");
    QVERIFY(lock.tryLock());
    manager->initializeForProfile(dir.path(), profileA, "Pet", {}, false);
    QVERIFY(!manager->persistenceWarning().isEmpty());
    manager->recordLlmAttempt(attempt("memory-only", QDateTime::currentDateTimeUtc()));
    manager->saveStatistics();
    QCOMPARE(count(manager->overview()["allTime"].toObject(), "callCount"), 1);
    QVERIFY(!QFile::exists(folder + "/statistics.json"));
    manager->initialize("", 0);
}

QTEST_MAIN(TestStatisticManager)
#include "test_statistic_manager.moc"
