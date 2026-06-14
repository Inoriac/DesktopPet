//
// Memory extractor / policy tests
//

#include <QTemporaryDir>
#include <QTest>

#include "memory/memory_extractor.h"
#include "memory/memory_policy.h"
#include "memory/memory_retriever.h"
#include "memory/memory_store.h"

class TestMemoryStrategy : public QObject {
    Q_OBJECT

private slots:
    void testExtractorCreatesPreferenceFromLikeStatement();
    void testExtractorCreatesSemanticFromRememberStatement();
    void testExtractorCreatesForgetCandidate();
    void testPolicyWritesAndSkipsDuplicate();
    void testPolicyRejectsSensitiveMemory();
    void testPolicyMarksMatchedMemoryDeleted();
    void testRetrieverRanksKeywordAndPreferredType();
    void testRetrieverFiltersSensitiveByDefault();
    void testRetrieverFormatsContextLines();
};

void TestMemoryStrategy::testExtractorCreatesPreferenceFromLikeStatement() {
    MemoryExtractor extractor;
    const QList<MemoryCandidate> candidates = extractor.extractFromUserInput(QStringLiteral("我喜欢 Java"), QStringLiteral("user_request"));

    QCOMPARE(candidates.size(), 1);
    QCOMPARE(candidates.first().operation, MemoryCandidateOperation::Write);
    QCOMPARE(candidates.first().entry.type, MemoryType::Preference);
    QCOMPARE(candidates.first().entry.privacyLevel, PrivacyLevel::Personal);
    QVERIFY(candidates.first().entry.summary.contains(QStringLiteral("用户喜欢")));
    QVERIFY(candidates.first().entry.summary.contains(QStringLiteral("Java")));
    QVERIFY(candidates.first().entry.tags.contains(QStringLiteral("preference")));
    QCOMPARE(candidates.first().entry.source, QStringLiteral("user_explicit"));
}

void TestMemoryStrategy::testExtractorCreatesSemanticFromRememberStatement() {
    MemoryExtractor extractor;
    const QList<MemoryCandidate> candidates = extractor.extractFromUserInput(
        QStringLiteral("请记住 Desktop-Pet 使用 C++20 和 Qt6"),
        QStringLiteral("user_request"));

    QCOMPARE(candidates.size(), 1);
    QCOMPARE(candidates.first().operation, MemoryCandidateOperation::Write);
    QCOMPARE(candidates.first().entry.type, MemoryType::Semantic);
    QCOMPARE(candidates.first().entry.scope, QStringLiteral("user"));
    QVERIFY(candidates.first().entry.summary.contains(QStringLiteral("Desktop-Pet")));
    QVERIFY(candidates.first().entry.evidence.contains(QStringLiteral("请记住 Desktop-Pet 使用 C++20 和 Qt6")));
}

void TestMemoryStrategy::testExtractorCreatesForgetCandidate() {
    MemoryExtractor extractor;
    const QList<MemoryCandidate> candidates = extractor.extractFromUserInput(QStringLiteral("删除关于面试的记忆"), QStringLiteral("user_request"));

    QCOMPARE(candidates.size(), 1);
    QCOMPARE(candidates.first().operation, MemoryCandidateOperation::Forget);
    QCOMPARE(candidates.first().query, QStringLiteral("面试"));
    QVERIFY(candidates.first().explicitRequest);
}

void TestMemoryStrategy::testPolicyWritesAndSkipsDuplicate() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    store.setStoragePath(tempDir.filePath(QStringLiteral("memory.json")));

    MemoryExtractor extractor;
    MemoryPolicy policy;
    const QList<MemoryCandidate> candidates = extractor.extractFromUserInput(QStringLiteral("我喜欢 C++"), QStringLiteral("user_request"));

    const MemoryPolicyReport firstReport = policy.applyCandidates(candidates, &store);
    QCOMPARE(firstReport.written, 1);
    QCOMPARE(firstReport.skipped, 0);
    QCOMPARE(store.all().size(), 1);
    QCOMPARE(store.all().first().status, MemoryStatus::Active);

    const MemoryPolicyReport secondReport = policy.applyCandidates(candidates, &store);
    QCOMPARE(secondReport.written, 0);
    QCOMPARE(secondReport.skipped, 1);
    QCOMPARE(store.all().size(), 1);
}

void TestMemoryStrategy::testPolicyRejectsSensitiveMemory() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    store.setStoragePath(tempDir.filePath(QStringLiteral("memory.json")));

    MemoryExtractor extractor;
    MemoryPolicy policy;
    const QList<MemoryCandidate> candidates = extractor.extractFromUserInput(QStringLiteral("请记住我的 api key 是 abc123"), QStringLiteral("user_request"));

    QCOMPARE(candidates.size(), 1);
    QCOMPARE(candidates.first().entry.privacyLevel, PrivacyLevel::Sensitive);

    const MemoryPolicyReport report = policy.applyCandidates(candidates, &store);
    QCOMPARE(report.written, 0);
    QCOMPARE(report.skipped, 1);
    QCOMPARE(store.all().size(), 0);
}

void TestMemoryStrategy::testPolicyMarksMatchedMemoryDeleted() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    store.setStoragePath(tempDir.filePath(QStringLiteral("memory.json")));

    MemoryExtractor extractor;
    MemoryPolicy policy;

    const MemoryPolicyReport writeReport = policy.applyCandidates(
        extractor.extractFromUserInput(QStringLiteral("我喜欢 Java"), QStringLiteral("user_request")),
        &store);
    QCOMPARE(writeReport.written, 1);
    QCOMPARE(store.all().size(), 1);

    const MemoryPolicyReport forgetReport = policy.applyCandidates(
        extractor.extractFromUserInput(QStringLiteral("忘记 Java"), QStringLiteral("user_request")),
        &store);
    QCOMPARE(forgetReport.forgotten, 1);
    QCOMPARE(store.all().size(), 1);
    QCOMPARE(store.all().first().status, MemoryStatus::Deleted);
}

void TestMemoryStrategy::testRetrieverRanksKeywordAndPreferredType() {
    MemoryStore store;

    MemoryEntry javaPreference;
    javaPreference.type = MemoryType::Preference;
    javaPreference.key = QStringLiteral("preference:java");
    javaPreference.summary = QStringLiteral("用户喜欢 Java");
    javaPreference.content = javaPreference.summary;
    javaPreference.tags = {QStringLiteral("preference"), QStringLiteral("coding")};
    javaPreference.scope = QStringLiteral("preference");
    javaPreference.importance = 0.8;
    javaPreference.confidence = 0.95;
    javaPreference.strength = 0.8;
    store.addEntry(javaPreference);

    MemoryEntry cppFact;
    cppFact.type = MemoryType::Semantic;
    cppFact.key = QStringLiteral("project:cpp");
    cppFact.summary = QStringLiteral("Desktop-Pet 使用 C++20 和 Qt6");
    cppFact.content = cppFact.summary;
    cppFact.tags = {QStringLiteral("project")};
    cppFact.scope = QStringLiteral("project");
    cppFact.importance = 0.7;
    cppFact.confidence = 0.9;
    cppFact.strength = 0.7;
    store.addEntry(cppFact);

    MemoryRetriever retriever;
    MemoryQuery query;
    query.text = QStringLiteral("你记得我喜欢 Java 吗");
    query.preferredTypes = {MemoryType::Preference};
    query.limit = 2;

    const QList<RetrievedMemory> result = retriever.retrieve(store, query);
    QVERIFY(result.size() >= 1);
    QCOMPARE(result.first().entry.type, MemoryType::Preference);
    QVERIFY(result.first().entry.summary.contains(QStringLiteral("Java")));
}

void TestMemoryStrategy::testRetrieverFiltersSensitiveByDefault() {
    MemoryStore store;

    MemoryEntry sensitive;
    sensitive.type = MemoryType::Semantic;
    sensitive.key = QStringLiteral("secret:token");
    sensitive.summary = QStringLiteral("用户的 token 是 abc");
    sensitive.content = sensitive.summary;
    sensitive.privacyLevel = PrivacyLevel::Sensitive;
    sensitive.importance = 1.0;
    sensitive.confidence = 1.0;
    store.addEntry(sensitive);

    MemoryRetriever retriever;
    MemoryQuery query;
    query.text = QStringLiteral("token");
    query.limit = 5;

    QVERIFY(retriever.retrieve(store, query).isEmpty());

    query.includeSensitive = true;
    QCOMPARE(retriever.retrieve(store, query).size(), 1);
}

void TestMemoryStrategy::testRetrieverFormatsContextLines() {
    MemoryStore store;

    MemoryEntry preference;
    preference.type = MemoryType::Preference;
    preference.key = QStringLiteral("preference:style");
    preference.summary = QStringLiteral("用户希望技术方案先讲架构，再讲代码");
    preference.content = preference.summary;
    preference.scope = QStringLiteral("communication");
    preference.importance = 0.9;
    preference.confidence = 0.96;
    preference.strength = 0.9;
    store.addEntry(preference);

    MemoryRetriever retriever;
    MemoryQuery query;
    query.text = QStringLiteral("技术方案怎么讲");
    query.preferredTypes = {MemoryType::Preference};
    query.limit = 1;

    const QStringList lines = retriever.formatForContext(retriever.retrieve(store, query));
    QCOMPARE(lines.size(), 1);
    QVERIFY(lines.first().startsWith(QStringLiteral("1. [preference/高置信度/communication]")));
    QVERIFY(lines.first().contains(QStringLiteral("先讲架构")));
}

QTEST_MAIN(TestMemoryStrategy)
#include "test_memory_strategy.moc"
