#include <QtTest>
#include "core/ai/memory/working_memory_cache.h"
#include "core/ai/memory/memory_cue_extractor.h"
#include "core/ai/memory/memory_keyword_index.h"
#include "core/ai/memory/actr_ranker.h"
#include "core/ai/memory/memory_retriever.h"
#include "core/ai/memory/memory_store.h"
#include "core/ai/memory/active_memory_pool.h"
#include "core/ai/memory/hippocampus_working_set.h"

class TestMemoryRecallPhase2 : public QObject {
    Q_OBJECT

private slots:
    void testMemoryCueExtractor();
    void testKeywordIndexBasics();
    void testKeywordIndexLookup();
    void testACTRBaseActivation();
    void testACTRCueMatch();
    void testACTRFullRanking();
    void testActivatedRetrievalIntegration();
};

void TestMemoryRecallPhase2::testMemoryCueExtractor() {
    MemoryCueExtractor extractor;
    
    extractor.setSessionContext("weather discussion", {"check_forecast"});
    extractor.setEmotionContext(EmotionType::Joy, 0.7);
    extractor.setPersonalityParameters(0.8, 0.5, 0.6);
    
    MemoryCue cue = extractor.extractFromQuery("今天天气 weather sunny");
    
    QVERIFY(!cue.normalizedQuery.isEmpty());
    QVERIFY(cue.tokens.size() > 0);
    QCOMPARE(cue.sessionTopic, QString("weather discussion"));
    QCOMPARE(cue.currentEmotion, EmotionType::Joy);
    QCOMPARE(cue.emotionIntensity, 0.7);
    QCOMPARE(cue.openness, 0.8);
}

void TestMemoryRecallPhase2::testKeywordIndexBasics() {
    MemoryKeywordIndex index;
    
    MemoryEntry entry1;
    entry1.id = "mem1";
    entry1.status = MemoryStatus::Active;
    entry1.privacyLevel = PrivacyLevel::Public;
    entry1.partition = "episodic";
    entry1.summary = "Weather is sunny today";
    entry1.tags = {"weather", "outdoor"};
    
    index.upsert(entry1);
    QCOMPARE(index.indexedCount(), 1);
    
    QSet<QString> tags = index.knownTags();
    QVERIFY(tags.contains("weather"));
    QVERIFY(tags.contains("outdoor"));
}

void TestMemoryRecallPhase2::testKeywordIndexLookup() {
    MemoryKeywordIndex index;
    
    MemoryEntry entry1;
    entry1.id = "mem1";
    entry1.status = MemoryStatus::Active;
    entry1.privacyLevel = PrivacyLevel::Public;
    entry1.partition = "episodic";
    entry1.summary = "Weather is sunny today";
    entry1.tags = {"weather"};
    
    MemoryEntry entry2;
    entry2.id = "mem2";
    entry2.status = MemoryStatus::Active;
    entry2.privacyLevel = PrivacyLevel::Public;
    entry2.partition = "episodic";
    entry2.summary = "Music preferences discussed";
    entry2.tags = {"music"};
    
    index.upsert(entry1);
    index.upsert(entry2);
    
    // Token lookup
    QList<QString> hits = index.lookup({"weather"}, {}, 10);
    QCOMPARE(hits.size(), 1);
    QCOMPARE(hits[0], QString("mem1"));
    
    // Tag lookup (higher weight)
    hits = index.lookup({}, {"music"}, 10);
    QCOMPARE(hits.size(), 1);
    QCOMPARE(hits[0], QString("mem2"));
}

void TestMemoryRecallPhase2::testACTRBaseActivation() {
    ACTRRanker ranker;
    
    MemoryEntry entry;
    entry.strength = 0.8;
    entry.importance = 0.6;
    entry.accessCount = 5;
    entry.lastAccessedAt = QDateTime::currentDateTimeUtc().addSecs(-3600);  // 1 hour ago
    
    double baseAct = ranker.computeBaseActivation(entry, QDateTime::currentDateTimeUtc());
    
    QVERIFY(baseAct > 0.0);
    QVERIFY(baseAct <= 1.0);
}

void TestMemoryRecallPhase2::testACTRCueMatch() {
    ACTRRanker ranker;
    
    MemoryEntry entry;
    entry.summary = "Weather is sunny today";
    entry.tags = {"weather", "outdoor"};
    
    MemoryCue cue;
    cue.tokens = {"weather", "sunny"};
    cue.knownTags = {"weather"};
    
    double cueMatch = ranker.computeCueMatch(entry, cue);
    
    QVERIFY(cueMatch > 0.0);
    QVERIFY(cueMatch <= 1.0);
}

void TestMemoryRecallPhase2::testACTRFullRanking() {
    ACTRRanker ranker;
    
    MemoryEntry entry1;
    entry1.id = "mem1";
    entry1.strength = 0.8;
    entry1.importance = 0.7;
    entry1.summary = "Weather sunny";
    
    MemoryEntry entry2;
    entry2.id = "mem2";
    entry2.strength = 0.5;
    entry2.importance = 0.4;
    entry2.summary = "Music discussion";
    
    CandidateMemory cand1;
    cand1.entry = entry1;
    cand1.runtimeActivation = 1.0;
    
    CandidateMemory cand2;
    cand2.entry = entry2;
    cand2.runtimeActivation = 0.3;
    
    MemoryCue cue;
    cue.tokens = {"weather"};
    
    QList<CandidateMemory> ranked = ranker.rank({cand1, cand2}, cue);
    
    QCOMPARE(ranked.size(), 2);
    // mem1 should rank higher (higher strength + importance + runtime + cue match)
    QCOMPARE(ranked[0].entry.id, QString("mem1"));
    QVERIFY(ranked[0].finalScore > ranked[1].finalScore);
}

void TestMemoryRecallPhase2::testActivatedRetrievalIntegration() {
    MemoryStore store;
    
    // Add some long-term memories
    MemoryEntry entry1;
    entry1.id = "ep1";
    entry1.type = MemoryType::Episodic;
    entry1.partition = "episodic";
    entry1.status = MemoryStatus::Active;
    entry1.summary = "Weather was sunny yesterday";
    entry1.tags = {"weather", "outdoor"};
    entry1.importance = 0.6;
    entry1.strength = 0.7;
    entry1.createdAt = QDateTime::currentDateTimeUtc().addDays(-1);
    store.addEntry(entry1);
    
    MemoryEntry entry2;
    entry2.id = "ep2";
    entry2.type = MemoryType::Episodic;
    entry2.partition = "episodic";
    entry2.status = MemoryStatus::Active;
    entry2.summary = "Music preferences discussed";
    entry2.tags = {"music"};
    entry2.importance = 0.5;
    entry2.strength = 0.6;
    entry2.createdAt = QDateTime::currentDateTimeUtc().addDays(-2);
    store.addEntry(entry2);
    
    // Add a hippocampus entry
    MemoryEntry entry3;
    entry3.id = "hc1";
    entry3.type = MemoryType::Working;
    entry3.partition = "hippocampus";
    entry3.status = MemoryStatus::Active;
    entry3.summary = "Just discussed weather forecast";
    entry3.tags = {"weather"};
    entry3.importance = 0.8;
    entry3.createdAt = QDateTime::currentDateTimeUtc();
    store.addEntry(entry3);
    
    // Setup channels
    ActiveMemoryPool activePool;
    activePool.activate("ep1", 1.0, "session");
    
    HippocampusWorkingSet workingSet(&store);
    workingSet.refresh();
    
    MemoryKeywordIndex keywordIndex;
    keywordIndex.rebuild(store.all());
    
    ActivationChannels channels;
    channels.activePool = &activePool;
    channels.workingSet = &workingSet;
    channels.keywordIndex = &keywordIndex;
    
    // Retrieve
    MemoryRetriever retriever;
    MemoryQuery query;
    query.text = "weather";
    query.limit = 5;
    
    QList<RetrievedMemory> results = retriever.retrieveActivated(store, query, channels);
    
    // Should find weather-related memories
    QVERIFY(results.size() > 0);
    
    bool foundWeather = false;
    for (const RetrievedMemory& mem : results) {
        if (mem.entry.tags.contains("weather")) {
            foundWeather = true;
            QVERIFY(!mem.sourceChannels.isEmpty());
        }
    }
    QVERIFY(foundWeather);
}

QTEST_MAIN(TestMemoryRecallPhase2)
#include "test_memory_recall_phase2.moc"
