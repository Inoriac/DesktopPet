#include <QtTest>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>

#include "ai/activity/activity_runtime.h"
#include "ai/chat/conversation_store.h"
#include "ai/event/event_ledger.h"
#include "ai/memory/memory_store.h"
#include "ai/model/model_router.h"
#include "ai/reflection/inner_thought_service.h"
#include "ai/reflection/private_key_provider.h"
#include "ai/reflection/private_psyche_crypto.h"
#include "ai/reflection/sqlite_private_psyche_repository.h"
#include "ai/task/task_experience_adapter.h"
#include "ai/task/task_store.h"

namespace {
const QString profileId = QStringLiteral("11111111-1111-4111-8111-111111111111");

class Ledger final : public EventLedger {
public:
    QMap<QString, EventRecord> events;
    Result<EventRecord, DomainError> append(const EventDraft& draft) override { return appendIdempotent(draft); }
    Result<EventRecord, DomainError> appendIdempotent(const EventDraft& draft) override {
        if (events.contains(draft.eventId)) return Result<EventRecord, DomainError>::success(events.value(draft.eventId));
        EventRecord record;
        static_cast<EventDraft&>(record) = draft;
        record.sequence = events.size() + 1;
        events.insert(record.eventId, record);
        return Result<EventRecord, DomainError>::success(record);
    }
    Result<QList<EventRecord>, DomainError> readAfter(qint64, const EventFilter&, int) const override {
        return Result<QList<EventRecord>, DomainError>::success(events.values());
    }
};

class DeferredClient final : public ModelCompletionClient {
public:
    QList<LlmCompletionHandler> pending;
    int requests = 0;
    void completeOnce(const ModelRouteConfig&, const QList<ChatMessage>&, const QJsonArray&,
                      LlmCompletionHandler callback, const QString&) override {
        ++requests;
        pending.append(std::move(callback));
    }
    void succeed() {
        auto callback = pending.takeFirst();
        LlmResponse response;
        response.content = QStringLiteral("{\"appraisal\":\"私密感受正文\",\"desire\":\"继续理解\",\"uncertainty\":\"用户反馈未知\",\"unresolved\":false}");
        callback(true, response, {});
    }
};

class KeyProvider final : public PrivateKeyProvider {
public:
    Result<PrivateKeyMaterial, DomainError> loadOrCreate(const QString& id) override {
        return Result<PrivateKeyMaterial, DomainError>::success({id, 1, QByteArray(32, 'k')});
    }
};

// Test double only; production uses authenticated Sodium encryption.
class TestCrypto final : public PrivatePsycheCrypto {
public:
    Result<EncryptedPrivatePayload, DomainError> encrypt(const QByteArray& data, const PrivateRecordAad&,
                                                       const PrivateKeyMaterial&) const override {
        EncryptedPrivatePayload payload;
        payload.nonce = QByteArrayLiteral("test-nonce");
        payload.ciphertext = data.toBase64();
        return Result<EncryptedPrivatePayload, DomainError>::success(payload);
    }
    Result<QByteArray, DomainError> decrypt(const EncryptedPrivatePayload& payload, const PrivateRecordAad&,
                                          const PrivateKeyMaterial&) const override {
        return Result<QByteArray, DomainError>::success(QByteArray::fromBase64(payload.ciphertext));
    }
};

ModelRoleConfig config() {
    ModelRouteConfig route;
    route.routeId = QStringLiteral("test");
    route.enabled = route.llm.enabled = true;
    route.llm.provider = QStringLiteral("test");
    route.llm.model = QStringLiteral("test-model");
    route.llm.baseUrl = QStringLiteral("https://example.test/v1");
    route.llm.apiKey = QStringLiteral("test-key");
    ModelRoleConfig role;
    role.role = ModelRole::FastExtract;
    role.routes = {route};
    return role;
}

struct Fixture {
    QTemporaryDir directory;
    TaskStore tasks;
    Ledger ledger;
    MemoryStore memories;
    DeferredClient client;
    ModelRoleRegistry registry{QList<ModelRoleConfig>{config()}};
    ModelRouter router{&registry, &client};
    KeyProvider keys;
    TestCrypto crypto;
    SqlitePrivatePsycheRepository privateStore;
    InnerThoughtService thoughts{profileId, &router, &keys, &crypto, &privateStore, &ledger};
    TaskRecord task;
    bool ready = false;

    Fixture(bool external = false) {
        memories.setDatabasePath(directory.filePath(QStringLiteral("memory.db")));
        ready =
            tasks.open(directory.filePath(QStringLiteral("runtime.db")),
                       profileId) &&
            memories.loadDatabaseOnly() &&
            privateStore.open(directory.filePath(QStringLiteral("private.db")))
                .isOk();
        task.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
        task.profileId = profileId;
        task.subjectId = QStringLiteral("owner");
        task.goal = QStringLiteral("比较两个目的地");
        task.status = QStringLiteral("Completed");
        task.result = QStringLiteral("基于资料的结果与限制");
        task.updatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        task.evidence = {QStringLiteral("user_input")};
        task.originKind = OriginKind::LocalOwner;
        task.accepted = true;
        task.requirementUpdates = QStringLiteral("优先比较交通便利程度");
        if (external) {
            task.status = "Failed";
            task.subjectId = "friend";
            task.originKind = OriginKind::External;
            task.context.profileId = profileId;
            task.context.originKind = OriginKind::External;
            task.context.channel = "chat";
            task.context.conversationId = "room";
            task.context.participantPersonIds = {"friend"};
        }
        ready = ready && tasks.save(task, true);
    }

    bool replayIntent() {
        const QString name = QUuid::createUuid().toString();
        bool ok = false;
        {
            auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            database.setDatabaseName(directory.filePath(QStringLiteral("runtime.db")));
            if (database.open()) {
                QSqlQuery query(database);
                ok = query.exec(
                    QStringLiteral("DELETE FROM agent_runtime_migrations WHERE "
                                   "migration_id LIKE 'experience:%'"));
            }
        }
        QSqlDatabase::removeDatabase(name);
        return ok;
    }
};
}

class TestTaskExperience : public QObject {
    Q_OBJECT
private slots:
  void subjectlessActivityRetainsVerifiedParticipantsAsParticipants() {
      QTemporaryDir directory;
      const auto path = directory.filePath("runtime.db");
      ActivityRuntime runtime(profileId, path);
      QSignalSpy ready(&runtime, &ActivityRuntime::ready);
      QTRY_COMPARE(ready.size(), 1);
      TaskDependencies dependencies;
      dependencies.requestModel = [](const TaskModelRequest &,
                                     TaskDependencies::ModelCompletion) {};
      runtime.setDependencies(dependencies);
      ConversationStore conversations(profileId, path);
      QVERIFY(conversations.initialize());
      HostConversationBinding binding;
      binding.channel = "chat";
      binding.connectionKey = "connection";
      binding.remoteConversationId = "group";
      binding.kind = ConversationKind::Group;
      QVERIFY(conversations.registerBinding(binding));
      IncomingMessage message;
      message.channel = binding.channel;
      message.connectionKey = binding.connectionKey;
      message.remoteConversationId = binding.remoteConversationId;
      message.kind = ConversationKind::Group;
      message.content = "Shared activity";
      message.occurredAt = QDateTime::currentDateTimeUtc();
      for (const auto &person : QStringList{"first", "second"}) {
          message.remoteMessageId = person;
          message.speakerIdentity = {"chat", "connection", person, person};
          QVERIFY(conversations.acceptMessage(message));
      }
      const auto conversationId =
          conversationIdFor(profileId, "chat", "connection", "group");
      const auto context = conversations.currentContext(conversationId);
      QVERIFY(context);
      ActivityIntent request;
      request.goal = "Read shared material";
      request.context = context.value();
      QString id;
      runtime.startActivityAsync(request, [&](auto result) {
          QVERIFY(result);
          id = result.value().activityId;
      });
      QTRY_VERIFY(!id.isEmpty());
      QTRY_VERIFY(runtime.isActive());
      ActivityStore store(profileId, path);
      const auto current = store.find(id);
      QVERIFY(current);
      ActivityTransition done;
      done.profileId = profileId;
      done.activityId = id;
      done.expectedRevision = current.value().revision;
      done.targetState = ActivityState::Failed;
      done.reason = "Source unavailable";
      bool ended = false;
      runtime.transitionActivityAsync(done, [&](auto result) {
          QVERIFY(result);
          ended = true;
      });
      QTRY_VERIFY(ended);
      MemoryStore memories;
      memories.setProfileId(profileId);
      memories.setDatabasePath(directory.filePath("memory.db"));
      QVERIFY(memories.loadDatabaseOnly());
      Ledger ledger;
      TaskExperienceAdapter adapter(profileId, &runtime, &ledger, &memories,
                                    nullptr);
      QSignalSpy drained(&adapter, &TaskExperienceAdapter::drained);
      adapter.drain();
      QTRY_COMPARE(drained.size(), 1);
      const auto memory = memories.readForRecall(id);
      QVERIFY(memory);
      QCOMPARE(memory->attribution.people.size(), 2);
      QVERIFY(memory->attribution.subjects().isEmpty());
      QVERIFY(memory->attribution.origins.first().speakerPersonId.isEmpty());
      QStringList people;
      for (const auto &person : memory->attribution.people) {
          QCOMPARE(person.role, MemoryPersonRole::Participant);
          people.append(person.personId);
      }
      people.sort();
      auto expected = context.value().participantPersonIds;
      expected.sort();
      QCOMPARE(people, expected);
      QCOMPARE(memory->attribution.audience.conversationIds,
               QStringList{conversationId});
      QVERIFY(ledger.events.value(id).occurredAt.isValid());
      const auto connection = QUuid::createUuid().toString();
      {
          auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
          db.setDatabaseName(memories.databasePath());
          QVERIFY(db.open());
          QSqlQuery query(db);
          query.prepare("SELECT COUNT(*) FROM memory_people WHERE memory_id=? "
                        "AND role='participant'");
          query.addBindValue(id);
          QVERIFY(query.exec());
          QVERIFY(query.next());
          QCOMPARE(query.value(0).toInt(), 2);
      }
      QSqlDatabase::removeDatabase(connection);
  }
  void externalExperience_preservesPeopleConversationAndAudience() {
      Fixture fixture(true);
      QVERIFY(fixture.ready);
      TaskExperienceAdapter adapter(profileId, &fixture.tasks, &fixture.ledger,
                                    &fixture.memories, nullptr);
      QSignalSpy drained(&adapter, &TaskExperienceAdapter::drained);
      adapter.drain();
      QTRY_COMPARE(drained.size(), 1);
      QCOMPARE(fixture.memories.all().size(), 1);
      const auto memory = fixture.memories.all().first();
      QCOMPARE(memory.attribution.audience.kind,
               MemoryAudienceKind::Conversation);
      QCOMPARE(memory.attribution.audience.conversationIds,
               QStringList{"room"});
      QCOMPARE(memory.attribution.origins.first().speakerPersonId, "friend");
      QCOMPARE(memory.attribution.origins.first().conversationId, "room");
      QVERIFY(memory.attribution.subjects().contains("friend"));
  }

    void unavailablePrivateStore_preservesFactAndPendingIntent() {
        Fixture fixture;
        QVERIFY(fixture.ready);
        TaskExperienceAdapter adapter(profileId, &fixture.tasks, &fixture.ledger, &fixture.memories, nullptr);
        QSignalSpy drained(&adapter, &TaskExperienceAdapter::drained);
        adapter.drain();
        QTRY_COMPARE(drained.size(), 1);
        QCOMPARE(fixture.tasks.pendingExperiences().size(), 1);
        QCOMPARE(fixture.memories.all().size(), 1);
        QCOMPARE(fixture.ledger.events.size(), 1);
        QCOMPARE(fixture.ledger.events.value(fixture.task.id).payload.value(QStringLiteral("feedback_status")).toString(), QStringLiteral("unknown"));
        QCOMPARE(fixture.ledger.events.value(fixture.task.id).payload.value(QStringLiteral("requirement_updates")).toString(), fixture.task.requirementUpdates);
        adapter.drain();
        QTRY_COMPARE(drained.size(), 2);
        QCOMPARE(fixture.memories.all().size(), 1);
        QCOMPARE(fixture.ledger.events.size(), 1);
    }

    void asynchronousReflection_andCrashReplay_doNotDuplicateExperience() {
        Fixture fixture;
        QVERIFY(fixture.ready);
        TaskExperienceAdapter adapter(profileId, &fixture.tasks, &fixture.ledger, &fixture.memories, &fixture.thoughts);
        QSignalSpy stored(&adapter, &TaskExperienceAdapter::experienceStored);
        QSignalSpy drained(&adapter, &TaskExperienceAdapter::drained);
        adapter.drain();
        QTRY_COMPARE(fixture.client.pending.size(), 1);
        QVERIFY(adapter.busy());
        QCOMPARE(fixture.memories.all().size(), 1);
        QCOMPARE(fixture.tasks.pendingExperiences().size(), 1);
        fixture.client.succeed();
        QTRY_COMPARE(stored.size(), 1);
        QTRY_COMPARE(drained.size(), 1);
        QVERIFY(fixture.tasks.pendingExperiences().isEmpty());
        QCOMPARE(fixture.privateStore.innerThoughtCount(profileId), 1);
        QVERIFY(fixture.replayIntent());
        adapter.drain();
        QTRY_COMPARE(stored.size(), 2);
        QCOMPARE(fixture.client.requests, 1);
        QCOMPARE(fixture.memories.all().size(), 1);
        QCOMPARE(fixture.privateStore.innerThoughtCount(profileId), 1);
        const auto factText = QJsonDocument(fixture.ledger.events.value(fixture.task.id).payload).toJson();
        QVERIFY(!factText.contains(QStringLiteral("私密感受正文").toUtf8()));
        QVERIFY(!QJsonDocument(fixture.memories.all().first().toJson()).toJson().contains(QStringLiteral("私密感受正文").toUtf8()));
    }

    void cancel_discardsLateReflectionAndRetainsRetryIntent() {
        Fixture fixture;
        QVERIFY(fixture.ready);
        TaskExperienceAdapter adapter(profileId, &fixture.tasks, &fixture.ledger, &fixture.memories, &fixture.thoughts);
        QSignalSpy stored(&adapter, &TaskExperienceAdapter::experienceStored);
        adapter.drain();
        QTRY_COMPARE(fixture.client.pending.size(), 1);
        adapter.cancel();
        fixture.client.succeed();
        QCOMPARE(stored.size(), 0);
        QCOMPARE(fixture.privateStore.innerThoughtCount(profileId), 0);
        QCOMPARE(fixture.tasks.pendingExperiences().size(), 1);
        QCOMPARE(fixture.memories.all().size(), 1);
    }
};

QTEST_MAIN(TestTaskExperience)
#include "test_task_experience.moc"
