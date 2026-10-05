#include <QtTest>
#include <limits>
#include "core/ai/memory/active_memory_pool.h"
#include "core/ai/memory/hippocampus_working_set.h"
#include "core/ai/memory/memory_store.h"

class TestMemoryRecall : public QObject {
    Q_OBJECT

private slots:
    void testActiveMemoryPoolBasics();
    void testActiveMemoryPoolDecay();
    void testActiveMemoryPoolOfflineRecovery();
    void testActiveMemoryPoolSnapshotBounds();
    void testHippocampusWorkingSetLoad();
    void testHippocampusWorkingSetScan();
    void testHippocampusContentAndRecencyPriority();
    void testHippocampusMetadataDoesNotSelectSeeds();
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

void TestMemoryRecall::testActiveMemoryPoolSnapshotBounds() {
    ActiveMemoryPool pool;
    pool.activate(QStringLiteral("negative"), -1.0, QStringLiteral("session"));
    QVERIFY(!pool.contains(QStringLiteral("negative")));
    pool.activate(QStringLiteral("nan-live"), std::numeric_limits<double>::quiet_NaN(),
                 QStringLiteral("session"));
    QVERIFY(!pool.contains(QStringLiteral("nan-live")));
    pool.activate(QStringLiteral("keep"), 1.0, QStringLiteral("session"));

    // A non-positive snapshot budget is an explicit request for no rows.
    QVERIFY(pool.snapshot(0).isEmpty());
    QVERIFY(pool.snapshot(-1).isEmpty());

    QList<ActiveMemoryItem> malformed;
    ActiveMemoryItem emptyId;
    emptyId.activation = 1.0;
    malformed.append(emptyId);
    ActiveMemoryItem nanItem;
    nanItem.memoryId = QStringLiteral("nan");
    nanItem.activation = std::numeric_limits<double>::quiet_NaN();
    malformed.append(nanItem);
    ActiveMemoryItem oversized;
    oversized.memoryId = QStringLiteral("oversized");
    oversized.activation = 100.0;
    malformed.append(oversized);

    ActiveMemoryPool restored;
    restored.restoreFromSnapshot(malformed, {}, {});
    QVERIFY(!restored.contains(QString()));
    QVERIFY(!restored.contains(QStringLiteral("nan")));
    QVERIFY(restored.contains(QStringLiteral("oversized")));
    QVERIFY(restored.getActivation(QStringLiteral("oversized")) <= 2.0);

    // Restore must retain the fixed pool budget even when persisted state is
    // larger than the live activation budget.
    QList<ActiveMemoryItem> many;
    for (int i = 0; i < ActiveMemoryPool::MAX_POOL_SIZE + 10; ++i) {
        ActiveMemoryItem item;
        item.memoryId = QStringLiteral("m%1").arg(i);
        item.activation = 0.1 + i * 0.01;
        item.source = QStringLiteral("session");
        many.append(item);
    }
    ActiveMemoryPool bounded;
    bounded.restoreFromSnapshot(many, {}, {});
    QCOMPARE(bounded.activeItems().size(), ActiveMemoryPool::MAX_POOL_SIZE);
    QVERIFY(!bounded.contains(QStringLiteral("m0")));
    QVERIFY(bounded.contains(QStringLiteral("m%1").arg(ActiveMemoryPool::MAX_POOL_SIZE + 9)));
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

    QVERIFY(workingSet.scan(QStringLiteral("weather"), {}, 0).isEmpty());
    QVERIFY(workingSet.scan(QStringLiteral("weather"), {}, -1).isEmpty());
    
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

void TestMemoryRecall::testHippocampusContentAndRecencyPriority() {
    MemoryStore store;
    const auto now = QDateTime::currentDateTimeUtc();
    const auto add = [&](const QString& id, const QString& text, int ageSeconds) {
        MemoryEntry entry;
        entry.id = id;
        entry.type = MemoryType::ShortTerm;
        entry.summary = text;
        entry.createdAt = entry.updatedAt = now.addSecs(-ageSeconds);
        return store.addEntry(entry);
    };
    // Partial coverage is 4/9; matching both words must beat recency alone.
    add("partial-new", "jazz", 0);           // 0.8 * 4/9 + 0.2 * 1
    add("full-old", "music jazz", 86400);    // 0.8 * 1 + 0.2 * 0 = .8
    add("full-recent", "music jazz", 3600);  // .8 + .2 * 23/24
    add("unrelated", "weather", -86400);
    HippocampusWorkingSet set(&store);
    QVERIFY(set.refresh());
    const auto hits = set.scan("jazz music", {}, 3);
    QCOMPARE(hits.size(), 3);
    QCOMPARE(hits[0].id, QString("full-recent"));
    QCOMPARE(hits[1].id, QString("full-old"));
    QCOMPARE(hits[2].id, QString("partial-new"));
    QCOMPARE(set.scan("jazz music", {}, 1).first().id, QString("full-recent"));

    // Future dates cap at 1 instead of growing an unlimited recency bonus.
    add("future-partial", "jazz", -86400 * 30);
    QVERIFY(set.refresh());
    QCOMPARE(set.scan("jazz music", {}, 1).first().id, QString("full-recent"));
    QCOMPARE(set.scan("", {}, 1).first().id, QString("future-partial"));
    // Chinese partial matches use the same bounded coverage.
    add("chinese", QString::fromUtf8("周末一起爬山"), 0);
    QVERIFY(set.refresh());
    QCOMPARE(set.scan(QString::fromUtf8("想去爬山"), {}, 1).first().id, QString("chinese"));
}

void TestMemoryRecall::testHippocampusMetadataDoesNotSelectSeeds() {
    MemoryStore store;
    const auto now = QDateTime::currentDateTimeUtc();
    MemoryEntry entry;
    entry.id = "a-low";
    entry.type = MemoryType::ShortTerm;
    entry.summary = "jazz music";
    entry.createdAt = entry.updatedAt = now;
    entry.importance = 0.1;
    entry.strength = 0.1;
    store.addEntry(entry);
    entry.id = "z-high";
    entry.importance = 1.0;
    entry.strength = 1.0;
    entry.emotion = EmotionType::Joy;
    entry.emotionIntensity = entry.emotionConfidence = 1.0;
    entry.mentionCount = entry.accessCount = 100;
    store.addEntry(entry);
    HippocampusWorkingSet set(&store);
    QVERIFY(set.refresh());
    // Equal relevance/recency resolves by ID, not salience.
    QCOMPARE(set.scan("jazz", {}, 1).first().id, QString("a-low"));

    entry.id = "old-high";
    entry.createdAt = entry.updatedAt = now.addSecs(-3600);
    store.addEntry(entry);
    set.setCapacity(2);
    QVERIFY(set.refresh());
    for (const auto& item : set.items()) QVERIFY(item.id != QString("old-high"));
}

QTEST_MAIN(TestMemoryRecall)
#include "test_memory_recall.moc"
