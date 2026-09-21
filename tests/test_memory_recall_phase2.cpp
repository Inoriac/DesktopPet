#include <QtTest>
#include <QTemporaryDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include "core/ai/memory/recall_text.h"
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
    void testWholeTagMatchingAndBoundaries();
    void testTextAndTagEvidenceStaySeparate();
    void testLexicalIdfAndSharedTokenization();
    void testGlobalTagMigrationAndEligibility();
    void testMemoryCueExtractor();
    void testKeywordIndexBasics();
    void testKeywordIndexLookup();
    void testACTRBaseActivation();
    void testACTRCueMatch();
    void testACTRSemanticCueAffectsRanking();
    void testACTRFullRanking();
    void testActivatedRetrievalIntegration();
};

void TestMemoryRecallPhase2::testWholeTagMatchingAndBoundaries() {
    MemoryCueExtractor extractor;
    extractor.setKnownTags({QStringLiteral("人工智能研究"), QStringLiteral("智能"),
        QStringLiteral("研究"), QStringLiteral("机器学习"), QStringLiteral("学习"),
        QStringLiteral("Ｃ＋＋"), QStringLiteral("Machine   Learning"), QStringLiteral("art")});
    const auto cue = extractor.extractFromQuery(QStringLiteral(
        "人工智能研究、机器学习和 C++，machine learning；party cart"));
    QCOMPARE(cue.knownTags.size(), 4);
    QVERIFY(cue.knownTags.contains(QStringLiteral("人工智能研究")));
    QVERIFY(cue.knownTags.contains(QStringLiteral("机器学习")));
    QVERIFY(cue.knownTags.contains(QStringLiteral("c++")));
    QVERIFY(cue.knownTags.contains(QStringLiteral("machine learning")));
    QVERIFY(!cue.knownTags.contains(QStringLiteral("智能")));
    QVERIFY(!cue.knownTags.contains(QStringLiteral("art")));
    QCOMPARE(extractor.extractFromQuery(QStringLiteral("ART" )).knownTags, QStringList{"art"});
    QCOMPARE(extractor.extractFromQuery(QStringLiteral("机器学习，机器学习" )).knownTags.size(), 1);
    extractor.setKnownTags({});
    QVERIFY(extractor.extractFromQuery(QStringLiteral("人工智能研究" )).knownTags.isEmpty());
}

void TestMemoryRecallPhase2::testTextAndTagEvidenceStaySeparate() {
    MemoryCueExtractor extractor;
    extractor.setKnownTags({QStringLiteral("机器学习")});
    const auto cue = extractor.extractFromQuery(QStringLiteral("机器学习"));
    CandidateMemory tagOnly;
    tagOnly.entry.id = "tag";
    tagOnly.entry.tags = {QStringLiteral("机器学习"), QStringLiteral("机器学习 ")};
    CandidateMemory textOnly;
    textOnly.entry.id = "text";
    textOnly.entry.summary = QStringLiteral("机器学习");
    CandidateMemory both = textOnly;
    both.entry.id = "both";
    both.entry.tags = tagOnly.entry.tags;
    const auto ranked = ACTRRanker().rank({tagOnly, textOnly, both}, cue);
    QCOMPARE(ranked.size(), 3);
    for (const auto& item : ranked) {
        QCOMPARE(item.cueMatch, ranked.first().cueMatch);
        if (item.entry.id == "tag") { QCOMPARE(item.lexicalCue, 0.0); QCOMPARE(item.tagCue, 1.0); }
        if (item.entry.id == "text") { QCOMPARE(item.lexicalCue, 1.0); QCOMPARE(item.tagCue, 0.0); }
    }
    // A token fragment inside a long label is not either kind of evidence.
    auto fragment = extractor.extractFromQuery(QStringLiteral("机器"));
    QCOMPARE(ACTRRanker().computeCueMatch(tagOnly.entry, fragment), 0.0);
    auto duplicateCue = cue;
    duplicateCue.tokens.append(cue.tokens);
    duplicateCue.knownTags.append(cue.knownTags);
    QCOMPARE(ACTRRanker().computeCueMatch(both.entry, duplicateCue), ranked.first().cueMatch);
    // Overlapping bigrams/trigrams cover the same text instead of stacking.
    auto bigrams = cue;
    bigrams.tokens = {QStringLiteral("机器"), QStringLiteral("器学"), QStringLiteral("学习")};
    QCOMPARE(ACTRRanker().computeCueMatch(textOnly.entry, bigrams), ranked.first().cueMatch);
}

void TestMemoryRecallPhase2::testLexicalIdfAndSharedTokenization() {
    MemoryKeywordIndex index;
    for (int i = 0; i < 10; ++i) {
        MemoryEntry entry;
        entry.id = QStringLiteral("doc-%1").arg(i);
        entry.type = MemoryType::Semantic;
        entry.summary = i == 9 ? QStringLiteral("quasar 人工智能研究") : QStringLiteral("today weather");
        index.upsert(entry);
    }
    auto cue = index.extractCue(QStringLiteral("today quasar"));
    QVERIFY(cue.tokenWeights.value("quasar") > cue.tokenWeights.value("today"));
    const auto hits = index.lookup(cue, 12);
    QCOMPARE(hits.first().memoryId, QStringLiteral("doc-9"));
    QCOMPARE(index.lookup({QStringLiteral("人工智")}, {}, 1).first(), QStringLiteral("doc-9"));
    QVERIFY(index.lookup({QStringLiteral("day")}, {}, 12).isEmpty());
}

void TestMemoryRecallPhase2::testGlobalTagMigrationAndEligibility() {
    QTemporaryDir directory;
    const auto path = directory.filePath("legacy.db");
    QString id;
    {
        MemoryStore legacy;
        legacy.setDatabasePath(path);
        QVERIFY(legacy.loadDatabaseOnly());
        MemoryEntry entry;
        entry.type = MemoryType::Semantic;
        entry.summary = "opaque evidence";
        entry.tags = {QStringLiteral("Ｍａｃｈｉｎｅ   Learning"), QStringLiteral("人工智能研究")};
        id = legacy.addEntry(entry).id;
        QVERIFY(!id.isEmpty());
        QSqlQuery query(QSqlDatabase::database(legacy.databaseConnectionName(), false));
        QVERIFY(query.exec("DROP INDEX idx_memory_tags_normalized"));
        QVERIFY(query.exec("ALTER TABLE memory_tags DROP COLUMN normalized_tag"));
    }
    MemoryStore store;
    store.setDatabasePath(path);
    QVERIFY(store.loadDatabaseOnly());
    MemoryKeywordIndex index; // No recent-window documents: tags must come from SQLite.
    QVERIFY(index.refreshGlobalTags(store.databaseConnectionName()));
    QVERIFY(index.knownTags().contains("machine learning"));
    auto cue = index.extractCue("machine learning");
    QCOMPARE(index.lookup(cue).first().memoryId, id);
    QCOMPARE(index.lookup(cue).first().lexicalScore, 0.0);
    QVERIFY(store.findById(id)->tags.contains(QStringLiteral("Ｍａｃｈｉｎｅ   Learning")));
    QSqlQuery revision(QSqlDatabase::database(store.databaseConnectionName(), false));
    QVERIFY(revision.exec("SELECT revision FROM memory_tag_catalog_state"));
    QVERIFY(revision.next());
    const auto before = revision.value(0).toLongLong();
    revision.finish();
    store.reinforceEntries({id});
    QVERIFY(revision.exec("SELECT revision FROM memory_tag_catalog_state"));
    QVERIFY(revision.next());
    QCOMPARE(revision.value(0).toLongLong(), before);
    revision.finish();
    // DB eligibility must override a dictionary cached before the mutation.
    QVERIFY(store.updateStatusById(id, MemoryStatus::Archived));
    QVERIFY(index.lookup(cue).isEmpty());
    QVERIFY(index.refreshGlobalTags(store.databaseConnectionName()));
    QVERIFY(!index.knownTags().contains("machine learning"));
    QVERIFY(store.updateStatusById(id, MemoryStatus::Active));
    QVERIFY(index.refreshGlobalTags(store.databaseConnectionName()));
    QVERIFY(index.knownTags().contains("machine learning"));
    auto changed = *store.findById(id);
    changed.expiresAt = QDateTime::currentDateTimeUtc().addSecs(-1);
    QVERIFY(store.updateEntryById(changed));
    QVERIFY(index.lookup(cue).isEmpty());
    changed.expiresAt = {};
    changed.privacyLevel = PrivacyLevel::Sensitive;
    QVERIFY(store.updateEntryById(changed));
    QVERIFY(index.lookup(cue).isEmpty());
    QVERIFY(index.refreshGlobalTags(store.databaseConnectionName()));
    QVERIFY(!index.knownTags().contains("machine learning"));
    changed.privacyLevel = PrivacyLevel::Personal;
    QVERIFY(store.updateEntryById(changed));
    QVERIFY(index.refreshGlobalTags(store.databaseConnectionName()));
    QVERIFY(index.knownTags().contains("machine learning"));
    QVERIFY(store.removeEntryById(id));
    QVERIFY(index.refreshGlobalTags(store.databaseConnectionName()));
    QVERIFY(index.knownTags().isEmpty());
    QSqlQuery plan(QSqlDatabase::database(store.databaseConnectionName(), false));
    QVERIFY(plan.exec("EXPLAIN QUERY PLAN SELECT memory_id FROM memory_tags WHERE normalized_tag='machine learning'"));
    QString details;
    while (plan.next()) details += plan.value(3).toString();
    QVERIFY2(details.contains("idx_memory_tags_normalized"), qPrintable(details));
}

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

void TestMemoryRecallPhase2::testACTRSemanticCueAffectsRanking() {
    ACTRRanker ranker;
    MemoryCue cue;

    CandidateMemory lexical;
    lexical.entry.id = "lexical";
    lexical.entry.strength = 0.5;
    lexical.entry.summary = "unrelated entry";

    CandidateMemory semantic;
    semantic.entry.id = "semantic";
    semantic.entry.strength = 0.5;
    semantic.entry.summary = "unrelated entry";
    semantic.semanticCue = 0.9;

    const QList<CandidateMemory> ranked = ranker.rank({lexical, semantic}, cue);
    QCOMPARE(ranked.size(), 2);
    QCOMPARE(ranked.first().entry.id, QStringLiteral("semantic"));
    QVERIFY(ranked.first().cueMatch > ranked.last().cueMatch);
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
