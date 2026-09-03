#include <QtTest>
#include "core/ai/memory/active_memory_pool.h"
#include "core/ai/memory/hippocampus_working_set.h"
#include "core/ai/memory/memory_store.h"

class TestMemoryRecall : public QObject {
    Q_OBJECT

private slots:
    void testActiveMemoryPoolBasics();
    void testActiveMemoryPoolDecay();
    void testActiveMemoryPoolOfflineRecovery();
    void testHippocampusWorkingSetLoad();
    void testHippocampusWorkingSetScan();
};

void TestMemoryRecall::testActiveMemoryPoolBasics() {
    ActiveMemoryPool pool;
    
    // Test activation
    pool.activate("mem1", 1.0, "session");
    QVERIFY(pool.contains("mem1"));
    QCOMPARE(pool.getActivation("mem1"), 1.0);
    
    // Test accumulation (capped at 2.0)
    pool.activate("mem1", 1.5, "session");
    QCOMPARE(pool.getActivation("mem1"), 2.0);
    
    // Test multiple items
    pool.activate("mem2", 0.5, "recent");
    pool.activate("mem3", 0.3, "emotion");
    
    QList<QString> ids = pool.activeMemoryIds(0.2);
    QCOMPARE(ids.size(), 3);
}

void TestMemoryRecall::testActiveMemoryPoolDecay() {
    ActiveMemoryPool pool;
    
    pool.activate("session_mem", 1.0, "session");  // 60 min half-life
    pool.activate("recent_mem", 1.0, "recent");    // 360 min half-life
    
    // Decay for 60 minutes (one half-life for session)
    pool.decay(60.0);
    
    QVERIFY(qAbs(pool.getActivation("session_mem") - 0.5) < 0.01);
    // Recent: 360 min half-life, 60 min decay = 0.891 (2^(-60/360))
    QVERIFY(pool.getActivation("recent_mem") > 0.88);
    
    // Test threshold removal
    pool.activate("weak_mem", 0.06, "session");
    pool.decay(60.0);  // After 60 min (1 half-life), 0.06 * 0.5 = 0.03 < 0.05
    QVERIFY(!pool.contains("weak_mem"));  // Should be removed below 0.05
}

void TestMemoryRecall::testActiveMemoryPoolOfflineRecovery() {
    ActiveMemoryPool pool;
    
    QDateTime savedAt = QDateTime::currentDateTimeUtc();
    
    pool.activate("mem1", 1.0, "session");
    pool.activate("mem2", 1.0, "goal");
    
    QList<ActiveMemoryItem> snapshot = pool.snapshot();
    QCOMPARE(snapshot.size(), 2);
    
    // Simulate 2 hours offline
    QDateTime now = savedAt.addSecs(7200);
    
    ActiveMemoryPool restored;
    restored.restoreFromSnapshot(snapshot, savedAt, now);
    
    // Session (60min half-life): 2 hours = 2 half-lives = 0.25x
    QVERIFY(restored.getActivation("mem1") < 0.3);
    QVERIFY(restored.getActivation("mem1") > 0.2);
    
    // Goal (24h half-life): 2 hours is small decay
    QVERIFY(restored.getActivation("mem2") > 0.9);
}

void TestMemoryRecall::testHippocampusWorkingSetLoad() {
    MemoryStore store;
    HippocampusWorkingSet workingSet(&store);
    
    // Add some hippocampus entries
    MemoryEntry entry1;
    entry1.id = "hc1";
    entry1.type = MemoryType::Working;
    entry1.partition = "hippocampus";
    entry1.status = MemoryStatus::Active;
    entry1.summary = "Recent interaction";
    entry1.importance = 5.0;
    entry1.createdAt = QDateTime::currentDateTimeUtc();
    store.addEntry(entry1);
    
    MemoryEntry entry2;
    entry2.id = "hc2";
    entry2.type = MemoryType::ShortTerm;
    entry2.partition = "hippocampus";
    entry2.status = MemoryStatus::Active;
    entry2.summary = "Another memory";
    entry2.importance = 3.0;
    entry2.createdAt = QDateTime::currentDateTimeUtc().addSecs(-3600);
    store.addEntry(entry2);
    
    // Add a non-hippocampus entry (should be ignored)
    MemoryEntry entry3;
    entry3.id = "ep1";
    entry3.type = MemoryType::Episodic;
    entry3.partition = "episodic";
    entry3.status = MemoryStatus::Active;
    entry3.summary = "Old memory";
    store.addEntry(entry3);
    
    workingSet.refresh();
    
    QCOMPARE(workingSet.size(), 2);
    QCOMPARE(workingSet.totalPendingCount(), 2);
}

void TestMemoryRecall::testHippocampusWorkingSetScan() {
    MemoryStore store;
    HippocampusWorkingSet workingSet(&store);
    
    MemoryEntry entry1;
    entry1.id = "hc1";
    entry1.type = MemoryType::Working;
    entry1.partition = "hippocampus";
    entry1.status = MemoryStatus::Active;
    entry1.summary = "Weather is sunny today";
    entry1.tags = {"weather", "outdoor"};
    entry1.createdAt = QDateTime::currentDateTimeUtc();
    store.addEntry(entry1);
    
    MemoryEntry entry2;
    entry2.id = "hc2";
    entry2.type = MemoryType::Working;
    entry2.partition = "hippocampus";
    entry2.status = MemoryStatus::Active;
    entry2.summary = "Music preferences discussed";
    entry2.tags = {"music", "preference"};
    entry2.createdAt = QDateTime::currentDateTimeUtc();
    store.addEntry(entry2);
    
    workingSet.refresh();
    
    // Text search
    QList<MemoryEntry> results = workingSet.scan("weather", {}, 10);
    QCOMPARE(results.size(), 1);
    QCOMPARE(results[0].id, QString("hc1"));
    
    // Tag filter
    results = workingSet.scan("", {"music"}, 10);
    QCOMPARE(results.size(), 1);
    QCOMPARE(results[0].id, QString("hc2"));
    
    // Combined
    results = workingSet.scan("", {"weather", "outdoor"}, 10);
    QCOMPARE(results.size(), 1);
}

QTEST_MAIN(TestMemoryRecall)
#include "test_memory_recall.moc"
