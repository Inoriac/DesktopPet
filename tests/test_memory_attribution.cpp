#include "memory/associative_activation_engine.h"
#include "memory/daydream_consolidator.h"
#include "memory/memory_attribution_service.h"
#include "memory/memory_keyword_index.h"
#include "memory/memory_metadata.h"
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

class MemoryAttributionTests : public QObject {
    Q_OBJECT
private slots:
    void sameSummaryDifferentFactsRemainSeparate();
    void reportedSubjectAndParticipantsSurviveConsolidation();
    void privateGraphBridgeAndWorkingMemoryAreInvisible();
    void ambiguousFirstPersonDoesNotClaimOwner();
    void ownActivitiesHaveVerifiedSubject();
    void summariesRespectCurrentAudience();
    void contextKeyUsesStableConversation();
    void migrationFailureKeepsCursorAndBody();
    void consolidationRejectsOtherSubjectAndLateOrigin();
    void sourceLookupFailureDoesNotWrite();
    void attributionReadFailureIsNotLegacy();
    void corruptAudienceIsNotLegacy_data();
    void corruptAudienceIsNotLegacy();
    void modelConflictCannotCrossPeople();
    void describedPeopleNeedCommittedReferences();
    void schemaUpgradePreservesBodyAndRejectsFutureVersion();
    void ingestTwoSpeakersAndOrigins();
    void pendingSourceAndMismatch();
    void unknownOriginalTimeRemainsNull();
    void audienceAndJsonRoundTrip();
    void migrationIsAtomicAndIdempotent();
    void associationFailureRollsBack();
    void contextSwitchKeepsTopics();
};
using SourceResult = Result<std::optional<MemoryOriginSource>, DomainError>;
static AttributedMemoryInput input(const QString& speaker, const QString& origin,
                                   const QString& conversation = "A") {
    AttributedMemoryInput i;
    i.entry.type = MemoryType::Preference;
    i.entry.summary = "我喜欢猫";
    i.entry.content = i.entry.summary;
    i.entry.confidence = 0.95;
    i.entry.key = "likes:cats";
    i.entry.scope = "user";
    i.attribution.profileId = "p";
    i.attribution.originId = origin;
    i.attribution.speakerPersonId = speaker;
    i.attribution.conversationId = conversation;
    i.attribution.audience = {MemoryAudienceKind::Conversation, {conversation}};
    i.attribution.claimKind = MemoryClaimKind::Fact;
    return i;
}
static MemoryOriginSource source(const AttributedMemoryInput& i) {
    MemoryOriginSource s;
    s.profileId = "p";
    s.originId = i.attribution.originId;
    s.speakerPersonId = i.attribution.speakerPersonId;
    s.conversationId = i.attribution.conversationId;
    s.text = i.entry.content;
    s.resolvedPeople = i.describedPeople;
    s.originKind = OriginKind::External;
    s.occurredAt = QDateTime::currentDateTimeUtc();
    return s;
}
void MemoryAttributionTests::ingestTwoSpeakersAndOrigins() {
    QTemporaryDir d;
    MemoryStore store;
    store.setProfileId("p");
    store.setDatabasePath(d.filePath("memory.db"));
    QVERIFY(store.loadDatabaseOnly());
    QHash<QString, MemoryOriginSource> sources;
    MemoryAttributionService service(
        "p", &store,
        [&](const QString&, const QString& id) {
            return SourceResult::success(sources.contains(id) ? std::optional(sources[id])
                                                              : std::nullopt);
        },
        [](const QString&, const QString&) { return Result<bool, DomainError>::success(true); });
    auto a = input("alice", "a"), b = input("bob", "b");
    sources["a"] = source(a);
    sources["b"] = source(b);
    QVERIFY(service.ingestMemory(a));
    QVERIFY(service.ingestMemory(b));
    QCOMPARE(store.all().size(), 2);
    QVERIFY(service.ingestMemory(a));
    QCOMPARE(store.all().size(), 2);
    a.attribution.originId = "a2";
    sources["a2"] = source(a);
    QVERIFY(service.ingestMemory(a));
    QCOMPARE(store.all().size(), 2);
    int origins = 0;
    for (const auto& e : store.all())
        origins += e.attribution.origins.size();
    QCOMPARE(origins, 3);
    auto negative = input("alice", "a3");
    negative.entry.content = negative.entry.summary = "我不喜欢猫";
    negative.entry.key = "dislikes:cats";
    sources["a3"] = source(negative);
    QVERIFY(service.ingestMemory(negative));
    QCOMPARE(store.all().size(), 3);
    const auto relations = store.relationGraph().all();
    int conflicts = 0;
    for (const auto& r : relations)
        if (r.type == MemoryRelationType::ConflictsWith) {
            ++conflicts;
            const auto from = store.readForRecall(r.fromMemoryId);
            const auto to = store.readForRecall(r.toMemoryId);
            QVERIFY(from);
            QVERIFY(to);
            QCOMPARE(from->attribution.subjects(), QStringList{"alice"});
            QCOMPARE(to->attribution.subjects(), QStringList{"alice"});
        }
    QCOMPARE(conflicts, 1);
}
void MemoryAttributionTests::unknownOriginalTimeRemainsNull() {
  QTemporaryDir d;
  MemoryStore store;
  store.setProfileId("p");
  store.setDatabasePath(d.filePath("m.db"));
  QVERIFY(store.loadDatabaseOnly());
  auto i = input("alice", "unknown-time");
  auto original = source(i);
  original.originalTimeKnown = false;
  MemoryAttributionService service(
      "p", &store,
      [&](const QString &, const QString &) {
        return SourceResult::success(original);
      },
      [](const QString &, const QString &) {
        return Result<bool, DomainError>::success(true);
      });
  QVERIFY(service.ingestMemory(i));
  QCOMPARE(store.all().size(), 1);
  QVERIFY(
      !store.all().first().attribution.origins.first().originalAt.isValid());
  const auto connection = QUuid::createUuid().toString();
  {
    auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
    db.setDatabaseName(d.filePath("m.db"));
    QVERIFY(db.open());
    QSqlQuery q(db);
    QVERIFY(q.exec("SELECT original_at FROM memory_origins WHERE "
                   "origin_id='unknown-time'"));
    QVERIFY(q.next());
    QVERIFY(q.value(0).isNull());
  }
  QSqlDatabase::removeDatabase(connection);
}
void MemoryAttributionTests::pendingSourceAndMismatch() {
    QTemporaryDir d;
    MemoryStore store;
    store.setProfileId("p");
    store.setDatabasePath(d.filePath("m.db"));
    QVERIFY(store.loadDatabaseOnly());
    auto i = input("alice", "a");
    std::optional<MemoryOriginSource> original;
    MemoryAttributionService s(
        "p", &store,
        [&](const QString&, const QString&) { return SourceResult::success(original); },
        [](const QString&, const QString&) { return Result<bool, DomainError>::success(true); });
    QVERIFY(s.ingestMemory(i));
    QCOMPARE(store.all().size(), 1);
    QVERIFY(!store.all()[0].attribution.resolved);
    QVERIFY(store.all()[0].attribution.people.isEmpty());
    original = source(i);
    QVERIFY(s.ingestMemory(i));
    QCOMPARE(store.all().size(), 1);
    QVERIFY(store.all()[0].attribution.resolved);
    QCOMPARE(store.all()[0].attribution.origins.size(), 1);
    i.attribution.speakerPersonId = "bob";
    auto mismatch = s.ingestMemory(i);
    QVERIFY(!mismatch);
    QCOMPARE(mismatch.error().code, QString("UNKNOWN_REFERENCE"));
}
void MemoryAttributionTests::audienceAndJsonRoundTrip() {
    QTemporaryDir d;
    MemoryStore store;
    store.setProfileId("p");
    store.setDatabasePath(d.filePath("m.db"));
    QVERIFY(store.loadDatabaseOnly());
    auto i = input("alice", "a");
    MemoryAttributionService s(
        "p", &store,
        [&](const QString&, const QString&) { return SourceResult::success(source(i)); },
        [](const QString&, const QString&) { return Result<bool, DomainError>::success(true); });
    QVERIFY(s.ingestMemory(i));
    auto e = store.all()[0];
    QCOMPARE(MemoryEntry::fromJson(e.toJson()).attribution.toJson(), e.attribution.toJson());
    QVERIFY(store.refreshDatabaseOnly());
    QCOMPARE(store.all()[0].attribution.toJson(), e.attribution.toJson());
    ContextSpec c;
    c.profileId = "p";
    c.originKind = OriginKind::External;
    c.conversationId = "B";
    QVERIFY(!memoryVisibleInContext(e.attribution, c));
    c.conversationId = "A";
    QVERIFY(memoryVisibleInContext(e.attribution, c));
    c.profileId = "q";
    QVERIFY(!memoryVisibleInContext(e.attribution, c));
    e.attribution.audience.kind = MemoryAudienceKind::Shareable;
    c.profileId = "p";
    c.conversationId = "B";
    QVERIFY(memoryVisibleInContext(e.attribution, c));
}
void MemoryAttributionTests::migrationIsAtomicAndIdempotent() {
    QTemporaryDir d;
    MemoryStore store;
    store.setDatabasePath(d.filePath("m.db"));
    QVERIFY(store.loadDatabaseOnly());
    MemoryEntry e;
    e.id = "old-a";
    e.summary = "用户喜欢猫";
    e.strength = 0.7;
    e.createdAt = QDateTime::fromString("2020-01-01T00:00:00Z", Qt::ISODate);
    e = store.addEntry(e);
    store.setProfileId("p");
    MemoryAttributionService s("p", &store);
    auto r = s.migrateAttribution({"p", {}, 1});
    QVERIFY(r);
    QVERIFY(r.value().complete);
    QVERIFY(store.refreshDatabaseOnly());
    auto migrated = store.all()[0];
    QCOMPARE(migrated.id, e.id);
    QCOMPARE(migrated.strength, e.strength);
    QCOMPARE(migrated.createdAt, e.createdAt);
    QVERIFY(!migrated.attribution.resolved);
    QCOMPARE(migrated.attribution.profileId, QString("p"));
    QVERIFY(s.migrateAttribution({"p", {}, 1}));
    QCOMPARE(store.all().size(), 1);
    auto stale = s.migrateAttribution({"p", "wrong", 1});
    QVERIFY(!stale);
    QCOMPARE(stale.error().code, QString("STALE_REVISION"));
}
void MemoryAttributionTests::associationFailureRollsBack() {
    QTemporaryDir d;
    MemoryStore store;
    store.setProfileId("p");
    store.setDatabasePath(d.filePath("m.db"));
    QVERIFY(store.loadDatabaseOnly());
    QSqlQuery q(QSqlDatabase::database(store.databaseConnectionName()));
    QVERIFY(q.exec("CREATE TRIGGER deny_origin BEFORE INSERT ON memory_origins BEGIN SELECT "
                   "RAISE(ABORT,'blocked'); END"));
    auto i = input("alice", "a");
    MemoryAttributionService s(
        "p", &store,
        [&](const QString&, const QString&) { return SourceResult::success(source(i)); },
        [](const QString&, const QString&) { return Result<bool, DomainError>::success(true); });
    QVERIFY(!s.ingestMemory(i));
    QVERIFY(store.refreshDatabaseOnly());
    QVERIFY(store.all().isEmpty());
}
void MemoryAttributionTests::contextSwitchKeepsTopics() {
    ActiveMemoryPool pool;
    PreparedEmbeddingQuery q;
    pool.selectContext("A");
    pool.recordRecall("猫", q, false, {{"a", 0.9, "session", QDateTime::currentDateTimeUtc(), {}}});
    pool.selectContext("B");
    QVERIFY(pool.topicItems().isEmpty());
    pool.recordRecall("书", q, false, {{"b", 0.9, "session", QDateTime::currentDateTimeUtc(), {}}});
    pool.selectContext("A");
    QCOMPARE(pool.topicText(), QString("猫"));
    QCOMPARE(pool.topicItems().size(), 1);
    QCOMPARE(pool.topicItems()[0].memoryId, QString("a"));
}

void MemoryAttributionTests::contextKeyUsesStableConversation() {
    auto context = ContextSpec::localOwner("p");
    context.activityId = "first";
    const auto key = context.cacheKey();
    context.activityId = "second";
    context.conversationRevision = 3;
    context.identityRevision = 4;
    QCOMPARE(context.cacheKey(), key);
    context.conversationId = "other";
    QVERIFY(context.cacheKey() != key);
    context.conversationId.clear();
    const auto activityKey = context.cacheKey();
    context.activityId = "third";
    QVERIFY(context.cacheKey() != activityKey);
    context.authorizedConversationIds = {"B", "A"};
    const auto authorizationKey = context.cacheKey();
    context.authorizedConversationIds = {"A", "B", "A"};
    QCOMPARE(context.cacheKey(), authorizationKey);
    context.allowOwnerMemory = true;
    QVERIFY(context.cacheKey() != authorizationKey);
}

void MemoryAttributionTests::summariesRespectCurrentAudience() {
    QTemporaryDir directory;
    MemoryStore store;
    store.setProfileId("p");
    store.setDatabasePath(directory.filePath("memory.db"));
    QVERIFY(store.loadDatabaseOnly());
    auto add = [&](const QString& id, MemoryAudienceKind audience, QString profile = "p") {
        MemoryEntry entry;
        entry.id = id;
        entry.summary = id;
        entry.attribution.profileId = profile;
        entry.attribution.audience = {audience, audience == MemoryAudienceKind::Conversation
                                                    ? QStringList{"A"}
                                                    : QStringList{}};
        return store.addEntry(entry);
    };
    add("owner-secret", MemoryAudienceKind::OwnerOnly);
    const auto shared = add("shared-fact", MemoryAudienceKind::Shareable);
    add("conversation-secret", MemoryAudienceKind::Conversation);
    add("other-profile", MemoryAudienceKind::Shareable, "q");
    const auto local = store.summaryForContext().join('\n');
    QVERIFY(local.contains("owner-secret"));
    QVERIFY(local.contains("shared-fact"));
    QVERIFY(!local.contains("conversation-secret"));
    QVERIFY(!local.contains("other-profile"));
    ContextSpec external;
    external.profileId = "p";
    external.originKind = OriginKind::External;
    external.conversationId = "A";
    auto summaries = store.summaryForContext(external).join('\n');
    QVERIFY(summaries.contains("conversation-secret"));
    QVERIFY(summaries.contains("shared-fact"));
    QVERIFY(!summaries.contains("owner-secret"));
    // Simulate another connection restricting visibility while this cache is stale.
    MemoryStore writer;
    writer.setDatabasePath(store.databasePath());
    QVERIFY(writer.loadDatabaseOnly());
    auto restricted = shared;
    restricted.attribution.audience = {MemoryAudienceKind::OwnerOnly, {}};
    QVERIFY(writer.updateEntryById(restricted));
    QVERIFY(!store.summaryForContext(external).join('\n').contains("shared-fact"));
    external.conversationId.clear();
    QVERIFY(store.summaryForContext(external).isEmpty());
}

void MemoryAttributionTests::ownActivitiesHaveVerifiedSubject() {
    for (const QString& text :
         {QStringLiteral("我最近在学习绘画"), QStringLiteral("我今天在公园散步")}) {
        auto candidate = input("owner", "original", "local_owner");
        candidate.entry.content = candidate.entry.summary = text;
        MemoryAttributionService service(
            "p", nullptr,
            [&](const QString&, const QString&) {
                return SourceResult::success(source(candidate));
            },
            [](const QString&, const QString&) {
                return Result<bool, DomainError>::success(true);
            });
        const auto attributed = service.attribute(candidate);
        QVERIFY(attributed);
        QCOMPARE(attributed.value().attribution.subjects(), QStringList{"owner"});
    }
}

void MemoryAttributionTests::sameSummaryDifferentFactsRemainSeparate() {
    QTemporaryDir directory;
    MemoryStore store;
    store.setProfileId("p");
    store.setDatabasePath(directory.filePath("memory.db"));
    QVERIFY(store.loadDatabaseOnly());
    auto first = input("alice", "first");
    auto second = input("alice", "second");
    second.entry.content = "我喜欢狗";
    second.entry.key = "likes:dogs";
    QHash<QString, MemoryOriginSource> sources{{"first", source(first)},
                                               {"second", source(second)}};
    MemoryAttributionService service(
        "p", &store,
        [&](const QString&, const QString& id) { return SourceResult::success(sources[id]); },
        [](const QString&, const QString&) { return Result<bool, DomainError>::success(true); });
    QVERIFY(service.ingestMemory(first));
    QVERIFY(service.ingestMemory(second));
    QCOMPARE(store.all().size(), 2);
    for (const auto& entry : store.all())
        QCOMPARE(entry.status, MemoryStatus::Active);
}

void MemoryAttributionTests::reportedSubjectAndParticipantsSurviveConsolidation() {
    QTemporaryDir directory;
    MemoryStore store;
    store.setProfileId("p");
    store.setDatabasePath(directory.filePath("memory.db"));
    QVERIFY(store.loadDatabaseOnly());
    auto reported = input("alice", "report");
    reported.entry.type = MemoryType::ShortTerm;
    reported.entry.summary = reported.entry.content = "Alice 说 Bob 喜欢猫";
    reported.attribution.claimKind = MemoryClaimKind::Reported;
    reported.describedPeople = {{"bob", MemoryPersonRole::Subject},
                                {"alice", MemoryPersonRole::Participant}};
    MemoryAttributionService service(
        "p", &store,
        [&](const QString&, const QString&) { return SourceResult::success(source(reported)); },
        [](const QString&, const QString&) { return Result<bool, DomainError>::success(true); });
    const auto write = service.ingestMemory(reported);
    QVERIFY(write);
    auto before = store.all().first();
    DaydreamConsolidator consolidator(store);
    DaydreamConsolidator::Decision decision;
    decision.sourceId = before.id;
    decision.action = DaydreamConsolidator::Action::Create;
    decision.targetType = MemoryType::Preference;
    decision.mergedContent = "Bob 喜欢猫（Alice 转述）";
    decision.qualityScore = 8;
    const auto stats = consolidator.applyDecisions({{before}}, {decision});
    QVERIFY(stats.committed);
    const auto original = store.readForRecall(before.id);
    QVERIFY(original);
    const auto derived = store.readForRecall(original->payload["consolidated_into"].toString());
    QVERIFY(derived);
    QCOMPARE(derived->attribution.subjects(), QStringList{"bob"});
    QCOMPARE(derived->attribution.people.size(), 2);
    QCOMPARE(derived->attribution.origins.first().speakerPersonId, QString("alice"));
    QCOMPARE(derived->attribution.origins.first().originId, QString("report"));
    QCOMPARE(derived->attribution.audience.toJson(), before.attribution.audience.toJson());
}

void MemoryAttributionTests::privateGraphBridgeAndWorkingMemoryAreInvisible() {
    QTemporaryDir directory;
    MemoryStore store;
    store.setProfileId("p");
    store.setDatabasePath(directory.filePath("memory.db"));
    QVERIFY(store.loadDatabaseOnly());
    auto make = [&](const QString& id, const QString& text, MemoryAudience audience) {
        MemoryEntry entry;
        entry.id = id;
        entry.type = MemoryType::Semantic;
        entry.summary = entry.content = text;
        entry.tags = {text};
        entry.attribution.profileId = "p";
        entry.attribution.audience = audience;
        return store.addEntry(entry);
    };
    const auto seed = make("seed", "猫", {MemoryAudienceKind::Shareable, {}});
    const auto hidden = make("hidden", "私密桥梁", {MemoryAudienceKind::Conversation, {"A"}});
    const auto destination = make("destination", "天气", {MemoryAudienceKind::Conversation, {"B"}});
    for (const auto& endpoints :
         {qMakePair(seed.id, hidden.id), qMakePair(hidden.id, destination.id)}) {
        MemoryRelation relation;
        relation.fromMemoryId = endpoints.first;
        relation.toMemoryId = endpoints.second;
        relation.type = MemoryRelationType::Related;
        relation.weight = 1.0;
        QVERIFY(store.relationGraph().addRelation(relation));
    }
    WorkingMemoryItem working;
    working.id = "private-temporary";
    working.summary = "猫 私密桥梁";
    working.attribution = hidden.attribution;
    QList<WorkingMemoryItem> temporary{working};
    MemoryKeywordIndex index;
    index.rebuild(store.all());
    ActiveMemoryPool pool;
    pool.activate(hidden.id, 1.5, "session");
    AssociativeActivationEngine graph;
    graph.setRandomSource([] { return 0.99; });
    ActivationChannels channels;
    channels.keywordIndex = &index;
    channels.activePool = &pool;
    channels.graphPropagation = &graph;
    channels.workingMemory = &temporary;
    MemoryQuery query;
    query.text = "猫";
    ContextSpec context;
    context.profileId = "p";
    context.originKind = OriginKind::External;
    context.conversationId = "B";
    query.context = context;
    const auto recalled = MemoryRetriever().retrieveWithGraphPropagation(store, query, channels);
    QVERIFY(!recalled.isEmpty());
    for (const auto& result : recalled) {
        QCOMPARE(result.entry.id, seed.id);
        QVERIFY(!result.reasons.join(' ').contains(hidden.id));
    }
    QCOMPARE(store.readForRecall(hidden.id)->accessCount, 0);
    QCOMPARE(pool.getActivation(hidden.id), 1.5);
    MemoryAttributionService service("p", &store);
    const auto shared = service.recallForContext({"猫", {}, context, 8});
    QVERIFY(shared);
    QVERIFY(!shared.value().isEmpty());
    for (const auto& result : shared.value())
        QCOMPARE(result.entry.id, seed.id);
    query.context.reset();
    query.text = "私密桥梁";
    QVERIFY(MemoryRetriever().retrieveWithGraphPropagation(store, query, channels).isEmpty());
}

void MemoryAttributionTests::ambiguousFirstPersonDoesNotClaimOwner() {
    for (const QString& text :
         {QStringLiteral("我的朋友小明喜欢猫"), QStringLiteral("我今天听甲说乙喜欢猫")}) {
        QTemporaryDir directory;
        MemoryStore store;
        store.setProfileId("p");
        store.setDatabasePath(directory.filePath("memory.db"));
        QVERIFY(store.loadDatabaseOnly());
        auto reported = input("owner", "report", "local_owner");
        reported.entry.summary = reported.entry.content = text;
        reported.attribution.claimKind = MemoryClaimKind::Reported;
        reported.attribution.inferSelfStatementClaim = true;
        auto original = source(reported);
        original.originKind = OriginKind::LocalOwner;
        MemoryAttributionService service(
            "p", &store,
            [&](const QString&, const QString&) { return SourceResult::success(original); },
            [](const QString&, const QString&) {
                return Result<bool, DomainError>::success(true);
            });
        QVERIFY(service.ingestMemory(reported));
        const auto memory = store.all().first();
        QVERIFY(memory.attribution.subjects().isEmpty());
        QVERIFY(!memory.attribution.resolved);
        QCOMPARE(memory.attribution.claimKind, MemoryClaimKind::Reported);
        QCOMPARE(memory.attribution.origins.first().speakerPersonId, QString("owner"));
    }
}

void MemoryAttributionTests::migrationFailureKeepsCursorAndBody() {
    QTemporaryDir directory;
    MemoryStore store;
    store.setDatabasePath(directory.filePath("memory.db"));
    QVERIFY(store.loadDatabaseOnly());
    for (const QString& id : {QString("a"), QString("b")}) {
        MemoryEntry entry;
        entry.id = id;
        entry.summary = "用户喜欢猫";
        QVERIFY(!store.addEntry(entry).id.isEmpty());
    }
    const auto original = store.readForRecall("b")->toJson();
    store.setProfileId("p");
    MemoryAttributionService service("p", &store);
    const auto first = service.migrateAttribution({"p", {}, 1});
    QVERIFY(first);
    QCOMPARE(first.value().nextCursor, QString("a"));
    QVERIFY(!first.value().complete);
    QSqlQuery query(QSqlDatabase::database(store.databaseConnectionName()));
    QVERIFY(query.exec("CREATE TRIGGER deny_second BEFORE INSERT ON memory_attribution WHEN "
                       "NEW.memory_id='b' BEGIN SELECT RAISE(ABORT,'blocked'); END"));
    QVERIFY(!service.migrateAttribution({"p", "a", 1}));
    QCOMPARE(store.readForRecall("b")->toJson(), original);
    QVERIFY(query.exec("SELECT cursor FROM agent_attribution_migrations WHERE profile_id='p'"));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toString(), QString("a"));
    query.finish();
    QVERIFY(query.exec("DROP TRIGGER deny_second"));
    const auto retry = service.migrateAttribution({"p", "a", 1});
    QVERIFY(retry);
    QVERIFY(retry.value().complete);
}

void MemoryAttributionTests::consolidationRejectsOtherSubjectAndLateOrigin() {
    QTemporaryDir directory;
    MemoryStore store;
    store.setProfileId("p");
    store.setDatabasePath(directory.filePath("memory.db"));
    QVERIFY(store.loadDatabaseOnly());
    auto target = input("bob", "b");
    auto incoming = input("alice", "a");
    incoming.entry.type = MemoryType::ShortTerm;
    QHash<QString, MemoryOriginSource> sources{{"a", source(incoming)}, {"b", source(target)}};
    MemoryAttributionService service(
        "p", &store,
        [&](const QString&, const QString& id) { return SourceResult::success(sources[id]); },
        [](const QString&, const QString&) { return Result<bool, DomainError>::success(true); });
    const auto targetWrite = service.ingestMemory(target);
    const auto incomingWrite = service.ingestMemory(incoming);
    QVERIFY(targetWrite);
    QVERIFY(incomingWrite);
    const auto sourceMemory = *store.readForRecall(incomingWrite.value().memoryIds.first());
    const auto targetMemory = *store.readForRecall(targetWrite.value().memoryIds.first());
    DaydreamConsolidator consolidator(store);
    DaydreamConsolidator::Decision decision;
    decision.sourceId = sourceMemory.id;
    decision.action = DaydreamConsolidator::Action::Update;
    decision.targetMemoryId = targetMemory.id;
    decision.expectedTarget = targetMemory;
    decision.targetType = MemoryType::Preference;
    decision.mergedContent = "我喜欢猫";
    decision.qualityScore = 8;
    const auto stats = consolidator.applyDecisions({{sourceMemory}}, {decision});
    QVERIFY(stats.committed);
    QCOMPARE(store.readForRecall(targetMemory.id)->attribution.subjects(), QStringList{"bob"});
    QCOMPARE(store.readForRecall(sourceMemory.id)->status, MemoryStatus::Active);
    decision.action = DaydreamConsolidator::Action::Create;
    const auto plan = consolidator.buildChangeSet({{sourceMemory}}, {decision});
    QVERIFY(plan);
    incoming.attribution.originId = "a2";
    sources["a2"] = source(incoming);
    QVERIFY(service.ingestMemory(incoming));
    const auto late = consolidator.applyChangeSet(plan.value());
    QVERIFY(!late.committed);
    QVERIFY(late.staleSnapshot);
    QCOMPARE(store.readForRecall(sourceMemory.id)->attribution.origins.size(), 2);
}

void MemoryAttributionTests::sourceLookupFailureDoesNotWrite() {
    QTemporaryDir directory;
    MemoryStore store;
    store.setProfileId("p");
    store.setDatabasePath(directory.filePath("memory.db"));
    QVERIFY(store.loadDatabaseOnly());
    MemoryAttributionService service("p", &store, [](const QString&, const QString&) {
        return SourceResult::failure(domainError("STORE_UNAVAILABLE", "source unavailable"));
    });
    const auto result = service.ingestMemory(input("alice", "a"));
    QVERIFY(!result);
    QCOMPARE(result.error().code, QString("STORE_UNAVAILABLE"));
    QVERIFY(store.all().isEmpty());
    MemoryStore unavailable;
    QVERIFY(!MemoryAttributionService("p", &unavailable).ingestMemory(input("alice", "a")));
}

void MemoryAttributionTests::attributionReadFailureIsNotLegacy() {
    QTemporaryDir directory;
    MemoryStore store;
    store.setProfileId("p");
    store.setDatabasePath(directory.filePath("memory.db"));
    QVERIFY(store.loadDatabaseOnly());
    MemoryEntry entry;
    entry.id = "hidden";
    entry.summary = "私密猫";
    entry.attribution.profileId = "p";
    entry.attribution.audience = {MemoryAudienceKind::Conversation, {"A"}};
    QVERIFY(!store.addEntry(entry).id.isEmpty());
    QSqlQuery q(QSqlDatabase::database(store.databaseConnectionName()));
    QVERIFY(q.exec("ALTER TABLE memory_attribution RENAME TO inaccessible_attribution"));
    MemoryAttributionService service("p", &store);
    const auto result = service.recallForContext({"私密猫", {}, ContextSpec::localOwner("p"), 8});
    QVERIFY(!result);
    QCOMPARE(result.error().code, QString("STORE_UNAVAILABLE"));
    QVERIFY(!store.readForRecall("hidden"));
}

void MemoryAttributionTests::corruptAudienceIsNotLegacy_data() {
    QTest::addColumn<QString>("audience");
    QTest::newRow("malformed") << QString("{");
    QTest::newRow("unknown-kind") << QString(R"({"kind":"unknown","conversationIds":[]})");
    QTest::newRow("wrong-array-type")
        << QString(R"({"kind":"conversation","conversationIds":"A"})");
    QTest::newRow("wrong-id-type") << QString(R"({"kind":"conversation","conversationIds":[1]})");
}

void MemoryAttributionTests::corruptAudienceIsNotLegacy() {
    QFETCH(QString, audience);
    QTemporaryDir directory;
    MemoryStore store;
    store.setProfileId("p");
    store.setDatabasePath(directory.filePath("memory.db"));
    QVERIFY(store.loadDatabaseOnly());
    MemoryEntry entry;
    entry.id = "hidden";
    entry.summary = "私密猫";
    entry.attribution.profileId = "p";
    entry.attribution.audience = {MemoryAudienceKind::Conversation, {"A"}};
    QVERIFY(!store.addEntry(entry).id.isEmpty());
    QSqlQuery query(QSqlDatabase::database(store.databaseConnectionName()));
    query.prepare("UPDATE memory_attribution SET audience_json=? WHERE memory_id='hidden'");
    query.addBindValue(audience);
    QVERIFY(query.exec());
    const auto result = MemoryAttributionService("p", &store)
                            .recallForContext({"私密猫", {}, ContextSpec::localOwner("p"), 8});
    QVERIFY(!result);
    QCOMPARE(result.error().code, QString("STORE_UNAVAILABLE"));
    QVERIFY(!store.readForRecall("hidden"));
    QVERIFY(store.summaryForContext().isEmpty());
    auto encoded = entry.toJson();
    auto attribution = encoded["attribution"].toObject();
    attribution["audience"] = QJsonDocument::fromJson(audience.toUtf8()).object();
    encoded["attribution"] = attribution;
    const auto invalid = MemoryEntry::fromJson(encoded);
    QVERIFY(!memoryVisibleInContext(invalid.attribution, ContextSpec::localOwner("p")));
    ContextSpec internal;
    internal.profileId = "p";
    internal.allowInternalMemory = internal.allowOwnerMemory = true;
    QVERIFY(!memoryVisibleInContext(invalid.attribution, internal));
}

void MemoryAttributionTests::schemaUpgradePreservesBodyAndRejectsFutureVersion() {
    QTemporaryDir directory;
    const auto path = directory.filePath("memory.db");
    QJsonObject original;
    {
        MemoryStore store;
        store.setDatabasePath(path);
        QVERIFY(store.loadDatabaseOnly());
        MemoryEntry entry;
        entry.id = "legacy";
        entry.summary = "旧正文";
        original = store.addEntry(entry).toJson();
        QSqlQuery q(QSqlDatabase::database(store.databaseConnectionName()));
        for (const auto& table : {"memory_origins", "memory_people", "memory_attribution",
                                  "agent_attribution_migrations"})
            QVERIFY(q.exec(QString("DROP TABLE %1").arg(table)));
        QVERIFY(q.exec("PRAGMA user_version=1"));
    }
    {
        MemoryStore migrated;
        migrated.setDatabasePath(path);
        QVERIFY(migrated.loadDatabaseOnly());
        QCOMPARE(migrated.readForRecall("legacy")->toJson(), original);
        QSqlQuery q(QSqlDatabase::database(migrated.databaseConnectionName()));
        QVERIFY(q.exec("PRAGMA user_version"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 2);
        q.finish();
        QVERIFY(q.exec("PRAGMA user_version=999"));
    }
    MemoryStore future;
    future.setDatabasePath(path);
    QString error;
    QVERIFY(!future.loadDatabaseOnly(&error));
    QVERIFY(error.contains("newer"));
}

void MemoryAttributionTests::modelConflictCannotCrossPeople() {
    QTemporaryDir directory;
    MemoryStore store;
    store.setProfileId("p");
    store.setDatabasePath(directory.filePath("memory.db"));
    QVERIFY(store.loadDatabaseOnly());
    auto add = [&](const QString& id, const QString& person, const QString& text) {
        MemoryEntry entry;
        entry.id = id;
        entry.type = MemoryType::Preference;
        entry.content = entry.summary = text;
        entry.attribution.profileId = "p";
        entry.attribution.resolved = true;
        entry.attribution.people = {{person, MemoryPersonRole::Subject}};
        return store.addEntry(entry);
    };
    const auto alice = add("alice", "alice", "我喜欢猫");
    const auto bob = add("bob", "bob", "我不喜欢猫");
    RelationProposal proposal;
    proposal.fromMemoryId = alice.id;
    proposal.toMemoryId = bob.id;
    proposal.type = MemoryRelationType::ConflictsWith;
    proposal.confidence = 0.99;
    proposal.evidence = "喜欢与不喜欢";
    QCOMPARE(
        DaydreamRelationReviewer().applyProposals({proposal}, {alice, bob}, store.relationGraph()),
        0);
    QVERIFY(store.relationGraph().all().isEmpty());
}

void MemoryAttributionTests::describedPeopleNeedCommittedReferences() {
    QTemporaryDir directory;
    MemoryStore store;
    store.setProfileId("p");
    store.setDatabasePath(directory.filePath("memory.db"));
    QVERIFY(store.loadDatabaseOnly());
    auto reported = input("alice", "report");
    reported.entry.content = reported.entry.summary = "有人喜欢猫";
    reported.describedPeople = {{"bob", MemoryPersonRole::Subject}};
    MemoryAttributionService service(
        "p", &store,
        [&](const QString&, const QString&) {
            auto original = source(reported);
            original.resolvedPeople.clear();
            return SourceResult::success(original);
        },
        [](const QString&, const QString&) { return Result<bool, DomainError>::success(true); });
    const auto result = service.ingestMemory(reported);
    QVERIFY(!result);
    QCOMPARE(result.error().code, QString("UNKNOWN_REFERENCE"));
    QVERIFY(store.all().isEmpty());
}

QTEST_GUILESS_MAIN(MemoryAttributionTests)
#include "test_memory_attribution.moc"
