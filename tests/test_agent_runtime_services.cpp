#include <QtTest>

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QVariant>

#include "ai/agent/agent_session.h"
#include "ai/ai_brain.h"
#include "ai/event/event_ledger.h"
#include "ai/memory/memory_store.h"
#include "ai/runtime/agent_bootstrap.h"
#include "ai/runtime/agent_runtime_services.h"
#include "ai/runtime/runtime_ui_bridge.h"
#include "configLoader/config_manager.h"

namespace {

const QString kProfileId = QStringLiteral("11111111-1111-4111-8111-111111111111");
const QString kOtherProfileId = QStringLiteral("33333333-3333-4333-8333-333333333333");

bool writeFile(const QString& path, const QByteArray& contents) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return file.write(contents) == contents.size();
}

bool execSql(const QString& databasePath, const QString& sql) {
    const QString connectionName = QStringLiteral("runtime_test_write_%1").arg(
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    bool ok = false;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), connectionName);
        database.setDatabaseName(databasePath);
        if (database.open()) {
            QSqlQuery query(database);
            ok = query.exec(sql);
            database.close();
        }
    }
    QSqlDatabase::removeDatabase(connectionName);
    return ok;
}

bool updateEventColumn(const QString& databasePath,
                       qint64 sequence,
                       const QString& column,
                       const QVariant& value) {
    static const QSet<QString> allowedColumns = {
        QStringLiteral("event_id"),
        QStringLiteral("schema_version"),
        QStringLiteral("profile_id"),
        QStringLiteral("type"),
        QStringLiteral("source"),
        QStringLiteral("privacy"),
        QStringLiteral("payload_json"),
        QStringLiteral("private_ref_json"),
        QStringLiteral("occurred_at"),
        QStringLiteral("created_at")
    };
    if (!allowedColumns.contains(column)) return false;

    const QString connectionName = QStringLiteral("runtime_test_update_%1").arg(
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    bool ok = false;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), connectionName);
        database.setDatabaseName(databasePath);
        if (database.open()) {
            QSqlQuery query(database);
            query.prepare(QStringLiteral("UPDATE event_log SET %1=? WHERE sequence=?")
                              .arg(column));
            query.addBindValue(value);
            query.addBindValue(sequence);
            ok = query.exec() && query.numRowsAffected() == 1;
            database.close();
        }
    }
    QSqlDatabase::removeDatabase(connectionName);
    return ok;
}

QString privateReferenceJson(
    const QString& store = QStringLiteral("private_psyche"),
    const QString& recordType = QStringLiteral("diary_entry"),
    const QString& recordId = QStringLiteral("22222222-2222-4222-8222-222222222222"),
    const QString& profileId = kProfileId) {
    return QString::fromUtf8(QJsonDocument(privateEventReferenceToJson({
        store, recordType, recordId, profileId
    })).toJson(QJsonDocument::Compact));
}

RuntimeStartRequest requestFor(QTemporaryDir& directory,
                               AIBrain* brain,
                               RuntimeUiBridge* bridge,
                               const QString& profileId = kProfileId) {
    RuntimeStartRequest request;
    request.profile = {QStringLiteral("Milltina"), QStringLiteral("model.gltf"), profileId};
    request.profileMigration.profileId = profileId;
    request.profileMigration.registeredProfileIds = {profileId};
    request.profileMigration.appDataRoot = directory.path();
    request.profileMigration.legacyDatabasePath =
        directory.filePath(QStringLiteral("legacy-memory.db"));
    request.profileMigration.legacyJsonPath =
        directory.filePath(QStringLiteral("legacy-memory.json"));
    request.configHash = QString(64, QLatin1Char('a'));
    request.identityBaselineSchemaVersion = 1;
    request.identityBaselineHash = QString(64, QLatin1Char('b'));
    request.aiBrain = brain;
    request.uiBridge = bridge;
    return request;
}

std::unique_ptr<CallbackRuntimeUiBridge> makeBridge(int* bubbleCalls = nullptr,
                                                    int* notificationCalls = nullptr) {
    RuntimeUiCallbacks callbacks;
    callbacks.showChatBubble = [bubbleCalls](const QString&, int) {
        if (bubbleCalls) ++(*bubbleCalls);
    };
    callbacks.notifyUser = [notificationCalls](const QString&, const QString&, int) {
        if (notificationCalls) ++(*notificationCalls);
    };
    return std::make_unique<CallbackRuntimeUiBridge>(
        std::move(callbacks),
        reinterpret_cast<AnimationPlayer*>(quintptr(1)),
        reinterpret_cast<AnimationManager*>(quintptr(2)));
}

EventDraft normalDraft(const QString& profileId = kProfileId) {
    EventDraft draft;
    draft.profileId = profileId;
    draft.type = QStringLiteral("UserMessageReceived");
    draft.source = QStringLiteral("test");
    draft.sessionId = QStringLiteral("session-read");
    draft.payload = {
        {QStringLiteral("text"), QStringLiteral("hello")},
        {QStringLiteral("triggerTag"), QStringLiteral("manual")}
    };
    return draft;
}

EventDraft privateDraft(const QString& profileId = kProfileId) {
    EventDraft draft;
    draft.profileId = profileId;
    draft.type = QStringLiteral("RuntimeDegraded");
    draft.source = QStringLiteral("reflection");
    draft.sessionId = QStringLiteral("session-read");
    draft.privacy = EventPrivacy::Private;
    draft.privateReference = PrivateEventReference{
        QStringLiteral("private_psyche"),
        QStringLiteral("diary_entry"),
        QStringLiteral("22222222-2222-4222-8222-222222222222"),
        profileId
    };
    return draft;
}

struct StartedRuntime {
    QTemporaryDir directory;
    AIBrain brain;
    std::unique_ptr<CallbackRuntimeUiBridge> bridge = makeBridge();
    AgentRuntimeServices services;
    RuntimeStartRequest request;
    Result<RuntimeStartReport, DomainError> result;

    StartedRuntime()
        : request(requestFor(directory, &brain, bridge.get()))
        , result(AgentBootstrap::start(services, request)) {}

    bool ready() const {
        return directory.isValid() && result.isOk()
            && result.value().capabilities.eventLedger;
    }
};

} // namespace

class TestAgentRuntimeServices : public QObject {
    Q_OBJECT

private slots:
    void readAfter_whenEventsMatchFilter_shouldReturnOrderedBoundedBatch();
    void readAfter_whenConsumerCanReadPrivateReference_shouldReturnReferenceWithoutBody();
    void readAfter_whenConsumerCannotReadPrivate_shouldFilterPrivateEvent();
    void readAfter_whenUnauthorizedPrivateRowIsCorrupted_shouldSkipItBeforeParsing();
    void readAfter_whenAuthorizedPrivateRowViolatesInvariant_shouldReject_data();
    void readAfter_whenAuthorizedPrivateRowViolatesInvariant_shouldReject();
    void readAfter_whenStoredEnvelopeIsInvalid_shouldReject_data();
    void readAfter_whenStoredEnvelopeIsInvalid_shouldReject();
    void readAfter_whenStoredEventBelongsToAnotherProfile_shouldRejectEntireRead();
    void readAfter_whenAuthorizationProfileDoesNotMatch_shouldRejectRead();

    void start_whenMigrationIsReady_shouldConfigureActivePathsBeforeFirstMemoryStoreOpen();
    void start_whenMigrationIsAmbiguous_shouldUseLegacyPathsAndDisableProfileGrowth();
    void start_whenNewSchemaMigrationFails_shouldKeepLegacyChatToolAndSkillPathAvailable();
    void initializeStorage_whenCalledTwice_shouldKeepFirstPathsAndRejectSecondCall();
    void initializeStorage_whenFirstAttemptFails_shouldRejectRetryWithoutSwitchingPaths();
    void triggerThink_whenStorageIsNotInitialized_shouldRemainInert();
    void triggerThink_whenUserRequestStorageIsNotInitialized_shouldRejectVisibly();

    void captureSnapshot_whenSessionStarts_shouldPinIdentityAndConfigVersions();
    void captureSnapshot_whenStateChangesLater_shouldKeepExistingSessionProjectionStable();
    void bindRuntimeSnapshot_whenSessionIdsMatch_shouldPinSnapshotOnce();
    void bindRuntimeSnapshot_whenCalledTwice_shouldPreserveOriginalSnapshot();

    void configHash_whenSameEffectiveJsonHasDifferentKeyOrder_shouldRemainDeterministic();
    void identityBaselineHash_whenBaselineChanges_shouldProduceDifferentVersionHash();

    void stop_whenRuntimeIsActive_shouldRejectNewSessionsAndCloseServices();
    void stop_whenCalledTwice_shouldRemainIdempotent();
    void start_whenBridgeIsValid_shouldUseUiCapabilitiesWithoutTakingOwnership();
    void stop_whenRuntimeEnds_shouldReleaseBridgeBeforePetWindowDestroysUiObjects();
};

void TestAgentRuntimeServices::readAfter_whenEventsMatchFilter_shouldReturnOrderedBoundedBatch() {
    StartedRuntime runtime;
    QVERIFY(runtime.ready());
    EventDraft first = normalDraft();
    EventDraft second = normalDraft();
    second.sessionId = QStringLiteral("other-session");
    EventDraft third = normalDraft();
    third.payload[QStringLiteral("text")] = QStringLiteral("third");
    QVERIFY(runtime.services.eventLedger()->append(first).isOk());
    QVERIFY(runtime.services.eventLedger()->append(second).isOk());
    QVERIFY(runtime.services.eventLedger()->append(third).isOk());
    auto authorization = runtime.services.authorizationFor(QStringLiteral("identity"));
    QVERIFY(authorization.isOk());
    EventFilter filter{{QStringLiteral("UserMessageReceived")},
                       QStringLiteral("session-read"), authorization.value()};

    const auto result = runtime.services.eventLedger()->readAfter(0, filter, 1);

    QVERIFY(result.isOk());
    QCOMPARE(result.value().size(), 1);
    QCOMPARE(result.value().first().sequence, 1);
}

void TestAgentRuntimeServices::readAfter_whenConsumerCanReadPrivateReference_shouldReturnReferenceWithoutBody() {
    StartedRuntime runtime;
    QVERIFY(runtime.ready());
    QVERIFY(runtime.services.eventLedger()->append(privateDraft()).isOk());
    auto authorization = runtime.services.authorizationFor(QStringLiteral("reflection"));
    QVERIFY(authorization.isOk());
    EventFilter filter{{}, {}, authorization.value()};

    const auto result = runtime.services.eventLedger()->readAfter(0, filter, 10);

    QVERIFY(result.isOk());
    QCOMPARE(result.value().size(), 1);
    QVERIFY(result.value().first().payload.isEmpty());
    QVERIFY(result.value().first().privateReference.has_value());
    QCOMPARE(result.value().first().privateReference->recordType,
             QStringLiteral("diary_entry"));
}

void TestAgentRuntimeServices::readAfter_whenConsumerCannotReadPrivate_shouldFilterPrivateEvent() {
    StartedRuntime runtime;
    QVERIFY(runtime.ready());
    QVERIFY(runtime.services.eventLedger()->append(normalDraft()).isOk());
    QVERIFY(runtime.services.eventLedger()->append(privateDraft()).isOk());
    auto authorization = runtime.services.authorizationFor(QStringLiteral("identity"));
    QVERIFY(authorization.isOk());
    EventFilter filter{{}, {}, authorization.value()};

    const auto result = runtime.services.eventLedger()->readAfter(0, filter, 10);

    QVERIFY(result.isOk());
    QCOMPARE(result.value().size(), 1);
    QCOMPARE(result.value().first().privacy, EventPrivacy::Normal);
}

void TestAgentRuntimeServices::readAfter_whenUnauthorizedPrivateRowIsCorrupted_shouldSkipItBeforeParsing() {
    StartedRuntime runtime;
    QVERIFY(runtime.ready());
    QVERIFY(runtime.services.eventLedger()->append(privateDraft()).isOk());
    QVERIFY(runtime.services.eventLedger()->append(normalDraft()).isOk());
    const QString runtimeDatabasePath = QDir(runtime.directory.path()).filePath(
        QStringLiteral("profiles/%1/agent_runtime.sqlite").arg(kProfileId));
    QVERIFY(execSql(runtimeDatabasePath, QStringLiteral(
        "UPDATE event_log SET private_ref_json='{invalid json}' WHERE privacy='private'")));
    auto authorization = runtime.services.authorizationFor(QStringLiteral("identity"));
    QVERIFY(authorization.isOk());
    EventFilter filter{{}, {}, authorization.value()};

    const auto result = runtime.services.eventLedger()->readAfter(0, filter, 1);

    QVERIFY(result.isOk());
    QCOMPARE(result.value().size(), 1);
    QCOMPARE(result.value().first().sequence, 2);
    QCOMPARE(result.value().first().privacy, EventPrivacy::Normal);
}

void TestAgentRuntimeServices::readAfter_whenAuthorizedPrivateRowViolatesInvariant_shouldReject_data() {
    QTest::addColumn<QString>("column");
    QTest::addColumn<QVariant>("value");

    QTest::newRow("invalid store")
        << QStringLiteral("private_ref_json")
        << QVariant(privateReferenceJson(QStringLiteral("memory")));
    QTest::newRow("invalid record type")
        << QStringLiteral("private_ref_json")
        << QVariant(privateReferenceJson(
               QStringLiteral("private_psyche"), QStringLiteral("unknown")));
    QTest::newRow("invalid record id")
        << QStringLiteral("private_ref_json")
        << QVariant(privateReferenceJson(
               QStringLiteral("private_psyche"), QStringLiteral("diary_entry"),
               QStringLiteral("not-a-uuid")));
    QTest::newRow("invalid reference profile id")
        << QStringLiteral("private_ref_json")
        << QVariant(privateReferenceJson(
               QStringLiteral("private_psyche"), QStringLiteral("diary_entry"),
               QStringLiteral("22222222-2222-4222-8222-222222222222"),
               QStringLiteral("not-a-uuid")));
    QTest::newRow("reference belongs to another profile")
        << QStringLiteral("private_ref_json")
        << QVariant(privateReferenceJson(
               QStringLiteral("private_psyche"), QStringLiteral("diary_entry"),
               QStringLiteral("22222222-2222-4222-8222-222222222222"),
               kOtherProfileId));
    QTest::newRow("private payload contains body")
        << QStringLiteral("payload_json")
        << QVariant(QStringLiteral("{\"body\":\"secret\"}"));
    QTest::newRow("non-private row contains reference")
        << QStringLiteral("privacy") << QVariant(QStringLiteral("normal"));
}

void TestAgentRuntimeServices::readAfter_whenAuthorizedPrivateRowViolatesInvariant_shouldReject() {
    QFETCH(QString, column);
    QFETCH(QVariant, value);
    StartedRuntime runtime;
    QVERIFY(runtime.ready());
    QVERIFY(runtime.services.eventLedger()->append(privateDraft()).isOk());
    const QString runtimeDatabasePath = QDir(runtime.directory.path()).filePath(
        QStringLiteral("profiles/%1/agent_runtime.sqlite").arg(kProfileId));
    QVERIFY(updateEventColumn(runtimeDatabasePath, 1, column, value));
    auto authorization = runtime.services.authorizationFor(QStringLiteral("reflection"));
    QVERIFY(authorization.isOk());
    EventFilter filter{{}, {}, authorization.value()};

    const auto result = runtime.services.eventLedger()->readAfter(0, filter, 10);

    QVERIFY(!result.isOk());
    QCOMPARE(result.error().code, QStringLiteral("EVT_SCHEMA_INVALID"));
}

void TestAgentRuntimeServices::readAfter_whenStoredEnvelopeIsInvalid_shouldReject_data() {
    QTest::addColumn<QString>("column");
    QTest::addColumn<QVariant>("value");

    QTest::newRow("empty event id")
        << QStringLiteral("event_id") << QVariant(QStringLiteral(""));
    QTest::newRow("invalid schema version")
        << QStringLiteral("schema_version") << QVariant(0);
    QTest::newRow("invalid profile id")
        << QStringLiteral("profile_id") << QVariant(QStringLiteral("not-a-uuid"));
    QTest::newRow("empty type")
        << QStringLiteral("type") << QVariant(QStringLiteral(""));
    QTest::newRow("empty source")
        << QStringLiteral("source") << QVariant(QStringLiteral(""));
    QTest::newRow("invalid privacy")
        << QStringLiteral("privacy") << QVariant(QStringLiteral("unknown"));
    QTest::newRow("invalid occurred-at")
        << QStringLiteral("occurred_at") << QVariant(QStringLiteral("not-a-date"));
    QTest::newRow("non-UTC occurred-at")
        << QStringLiteral("occurred_at")
        << QVariant(QStringLiteral("2026-08-24T12:00:00.000+08:00"));
    QTest::newRow("invalid created-at")
        << QStringLiteral("created_at") << QVariant(QStringLiteral("not-a-date"));
    QTest::newRow("non-UTC created-at")
        << QStringLiteral("created_at")
        << QVariant(QStringLiteral("2026-08-24T12:00:00.000+08:00"));
    QTest::newRow("payload violates registered schema")
        << QStringLiteral("payload_json") << QVariant(QStringLiteral("{}"));
}

void TestAgentRuntimeServices::readAfter_whenStoredEnvelopeIsInvalid_shouldReject() {
    QFETCH(QString, column);
    QFETCH(QVariant, value);
    StartedRuntime runtime;
    QVERIFY(runtime.ready());
    QVERIFY(runtime.services.eventLedger()->append(normalDraft()).isOk());
    const QString runtimeDatabasePath = QDir(runtime.directory.path()).filePath(
        QStringLiteral("profiles/%1/agent_runtime.sqlite").arg(kProfileId));
    QVERIFY(updateEventColumn(runtimeDatabasePath, 1, column, value));
    auto authorization = runtime.services.authorizationFor(QStringLiteral("identity"));
    QVERIFY(authorization.isOk());
    EventFilter filter{{}, {}, authorization.value()};

    const auto result = runtime.services.eventLedger()->readAfter(0, filter, 10);

    QVERIFY(!result.isOk());
    QCOMPARE(result.error().code, QStringLiteral("EVT_SCHEMA_INVALID"));
}

void TestAgentRuntimeServices::readAfter_whenStoredEventBelongsToAnotherProfile_shouldRejectEntireRead() {
    StartedRuntime runtime;
    QVERIFY(runtime.ready());
    QVERIFY(runtime.services.eventLedger()->append(normalDraft()).isOk());
    QVERIFY(runtime.services.eventLedger()->append(normalDraft()).isOk());
    const QString runtimeDatabasePath = QDir(runtime.directory.path()).filePath(
        QStringLiteral("profiles/%1/agent_runtime.sqlite").arg(kProfileId));
    QVERIFY(updateEventColumn(runtimeDatabasePath, 2, QStringLiteral("profile_id"),
                              kOtherProfileId));
    auto authorization = runtime.services.authorizationFor(QStringLiteral("identity"));
    QVERIFY(authorization.isOk());
    EventFilter filter{{}, {}, authorization.value()};

    const auto result = runtime.services.eventLedger()->readAfter(0, filter, 10);

    QVERIFY(!result.isOk());
    QCOMPARE(result.error().code, QStringLiteral("EVT_READ_FORBIDDEN"));
}

void TestAgentRuntimeServices::readAfter_whenAuthorizationProfileDoesNotMatch_shouldRejectRead() {
    StartedRuntime runtime;
    QVERIFY(runtime.ready());
    QTemporaryDir otherDirectory;
    AIBrain otherBrain;
    auto otherBridge = makeBridge();
    AgentRuntimeServices otherServices;
    RuntimeStartRequest otherRequest =
        requestFor(otherDirectory, &otherBrain, otherBridge.get(), kOtherProfileId);
    auto otherStarted = AgentBootstrap::start(otherServices, otherRequest);
    QVERIFY(otherStarted.isOk());
    auto otherAuthorization = otherServices.authorizationFor(QStringLiteral("identity"));
    QVERIFY(otherAuthorization.isOk());
    EventFilter filter{{}, {}, otherAuthorization.value()};

    const auto result = runtime.services.eventLedger()->readAfter(0, filter, 10);

    QVERIFY(!result.isOk());
    QCOMPARE(result.error().code, QStringLiteral("EVT_READ_FORBIDDEN"));
}

void TestAgentRuntimeServices::start_whenMigrationIsReady_shouldConfigureActivePathsBeforeFirstMemoryStoreOpen() {
    QTemporaryDir directory;
    AIBrain brain;
    QVERIFY(!brain.isStorageInitialized());
    auto bridge = makeBridge();
    AgentRuntimeServices services;
    const RuntimeStartRequest request = requestFor(directory, &brain, bridge.get());

    const auto result = AgentBootstrap::start(services, request);

    QVERIFY(result.isOk());
    QVERIFY(brain.isStorageInitialized());
    QCOMPARE(brain.memoryStore()->databasePath(),
             QDir(directory.path()).filePath(
                 QStringLiteral("profiles/%1/memory.db").arg(kProfileId)));
    QCOMPARE(result.value().mode, RuntimeMode::Running);
    QVERIFY(result.value().capabilities.profileStore);
    QVERIFY(result.value().capabilities.eventLedger);
}

void TestAgentRuntimeServices::start_whenMigrationIsAmbiguous_shouldUseLegacyPathsAndDisableProfileGrowth() {
    QTemporaryDir directory;
    QVERIFY(writeFile(directory.filePath(QStringLiteral("legacy-memory.json")), "[]"));
    AIBrain brain;
    auto bridge = makeBridge();
    AgentRuntimeServices services;
    RuntimeStartRequest request = requestFor(directory, &brain, bridge.get());
    request.profileMigration.registeredProfileIds.append(kOtherProfileId);

    const auto result = AgentBootstrap::start(services, request);

    QVERIFY(result.isOk());
    QCOMPARE(result.value().profileMigration.status, ProfileMigrationStatus::Ambiguous);
    QCOMPARE(result.value().mode, RuntimeMode::Degraded);
    QVERIFY(!result.value().capabilities.profileStore);
    QVERIFY(!result.value().capabilities.profileGrowth);
    QCOMPARE(brain.memoryStore()->databasePath(), request.profileMigration.legacyDatabasePath);
}

void TestAgentRuntimeServices::start_whenNewSchemaMigrationFails_shouldKeepLegacyChatToolAndSkillPathAvailable() {
    QTemporaryDir directory;
    const QString profileRoot = QDir(directory.path()).filePath(
        QStringLiteral("profiles/%1").arg(kProfileId));
    QVERIFY(QDir().mkpath(profileRoot));
    {
        MemoryStore existing;
        existing.setDatabasePath(QDir(profileRoot).filePath(QStringLiteral("memory.db")));
        existing.setStoragePath(QDir(profileRoot).filePath(QStringLiteral("memory.json")));
        QVERIFY(existing.load());
    }
    QVERIFY(QDir().mkpath(QDir(profileRoot).filePath(QStringLiteral("agent_runtime.sqlite"))));
    AIBrain brain;
    auto bridge = makeBridge();
    AgentRuntimeServices services;
    RuntimeStartRequest request = requestFor(directory, &brain, bridge.get());

    const auto result = AgentBootstrap::start(services, request);

    QVERIFY(result.isOk());
    QCOMPARE(result.value().mode, RuntimeMode::Degraded);
    QVERIFY(brain.isStorageInitialized());
    QVERIFY(!result.value().capabilities.eventLedger);
    QVERIFY(brain.memoryStore() != nullptr);
    QVERIFY(brain.skillStore() != nullptr);
}

void TestAgentRuntimeServices::initializeStorage_whenCalledTwice_shouldKeepFirstPathsAndRejectSecondCall() {
    QTemporaryDir directory;
    AIBrain brain;
    const AIBrainStorageConfig first{
        directory.filePath(QStringLiteral("first.db")),
        directory.filePath(QStringLiteral("first.json"))
    };
    const AIBrainStorageConfig second{
        directory.filePath(QStringLiteral("second.db")),
        directory.filePath(QStringLiteral("second.json"))
    };
    QVERIFY(brain.initializeStorage(first).isOk());

    const auto result = brain.initializeStorage(second);

    QVERIFY(!result.isOk());
    QCOMPARE(result.error().code, QStringLiteral("STATE_VERSION_CONFLICT"));
    QCOMPARE(brain.memoryStore()->databasePath(), first.databasePath);
    QCOMPARE(brain.memoryStore()->storagePath(), first.jsonPath);
}

void TestAgentRuntimeServices::initializeStorage_whenFirstAttemptFails_shouldRejectRetryWithoutSwitchingPaths() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString blockedParent = directory.filePath(QStringLiteral("not-a-directory"));
    QVERIFY(writeFile(blockedParent, "blocked"));
    AIBrain brain;
    const AIBrainStorageConfig first{
        QDir(blockedParent).filePath(QStringLiteral("memory.db")),
        directory.filePath(QStringLiteral("first.json"))
    };
    const AIBrainStorageConfig second{
        directory.filePath(QStringLiteral("second.db")),
        directory.filePath(QStringLiteral("second.json"))
    };
    const auto firstResult = brain.initializeStorage(first);
    QVERIFY(!firstResult.isOk());
    QCOMPARE(firstResult.error().code, QStringLiteral("MEMORY_STORE_UNAVAILABLE"));

    const auto retryResult = brain.initializeStorage(second);

    QVERIFY(!retryResult.isOk());
    QCOMPARE(retryResult.error().code, QStringLiteral("STATE_VERSION_CONFLICT"));
    QVERIFY(!brain.isStorageInitialized());
    QCOMPARE(brain.memoryStore()->databasePath(), first.databasePath);
    QCOMPARE(brain.memoryStore()->storagePath(), first.jsonPath);
}

void TestAgentRuntimeServices::triggerThink_whenStorageIsNotInitialized_shouldRemainInert() {
    AIBrain brain;
    QSignalSpy thinkingStarted(&brain, &AIBrain::thinkingStarted);
    const int memoryCount = brain.memoryStore()->all().size();

    brain.triggerThink(QStringLiteral("pre-bootstrap probe"),
                       QStringLiteral("idle_action"));

    QCOMPARE(brain.memoryStore()->all().size(), memoryCount);
    QCOMPARE(thinkingStarted.count(), 0);
}

void TestAgentRuntimeServices::triggerThink_whenUserRequestStorageIsNotInitialized_shouldRejectVisibly() {
    AIBrain brain;
    QSignalSpy rejected(&brain, &AIBrain::thinkRequestRejected);

    brain.triggerThink(QStringLiteral("hello"),
                       QStringLiteral("user_request"),
                       QStringLiteral("user-message-id"));

    QCOMPARE(rejected.count(), 1);
    QCOMPARE(rejected.first().at(0).toString(), QStringLiteral("user-message-id"));
    QVERIFY(!rejected.first().at(1).toString().isEmpty());
    QVERIFY(!brain.isBusy());
}

void TestAgentRuntimeServices::captureSnapshot_whenSessionStarts_shouldPinIdentityAndConfigVersions() {
    StartedRuntime runtime;
    QVERIFY(runtime.ready());

    const RuntimeSnapshot snapshot =
        runtime.services.captureSnapshot(QStringLiteral("session-1"));

    QCOMPARE(snapshot.sessionId, QStringLiteral("session-1"));
    QCOMPARE(snapshot.profileId, kProfileId);
    QCOMPARE(snapshot.identityBaselineSchemaVersion, 1);
    QCOMPARE(snapshot.identityBaselineHash, runtime.request.identityBaselineHash);
    QCOMPARE(snapshot.configHash, runtime.request.configHash);
    QVERIFY(snapshot.capturedAt.isValid());
}

void TestAgentRuntimeServices::captureSnapshot_whenStateChangesLater_shouldKeepExistingSessionProjectionStable() {
    StartedRuntime runtime;
    QVERIFY(runtime.ready());
    AgentSession session = AgentSession::create(QStringLiteral("hello"), QStringLiteral("manual"));
    const RuntimeSnapshot original = runtime.services.captureSnapshot(session.id());
    QVERIFY(session.bindRuntimeSnapshot(original).isOk());
    runtime.request.configHash = QString(64, QLatin1Char('c'));
    runtime.request.identityBaselineHash = QString(64, QLatin1Char('d'));

    QCOMPARE(session.runtimeSnapshot()->configHash, original.configHash);
    QCOMPARE(session.runtimeSnapshot()->identityBaselineHash, original.identityBaselineHash);
}

void TestAgentRuntimeServices::bindRuntimeSnapshot_whenSessionIdsMatch_shouldPinSnapshotOnce() {
    AgentSession session = AgentSession::create(QStringLiteral("hello"), QStringLiteral("manual"));
    RuntimeSnapshot snapshot;
    snapshot.sessionId = session.id();
    snapshot.profileId = kProfileId;

    QVERIFY(session.bindRuntimeSnapshot(snapshot).isOk());
    QVERIFY(session.runtimeSnapshot().has_value());
    QCOMPARE(session.runtimeSnapshot()->profileId, kProfileId);
}

void TestAgentRuntimeServices::bindRuntimeSnapshot_whenCalledTwice_shouldPreserveOriginalSnapshot() {
    AgentSession session = AgentSession::create(QStringLiteral("hello"), QStringLiteral("manual"));
    RuntimeSnapshot first;
    first.sessionId = session.id();
    first.profileId = kProfileId;
    RuntimeSnapshot second = first;
    second.profileId = kOtherProfileId;
    QVERIFY(session.bindRuntimeSnapshot(first).isOk());

    const auto result = session.bindRuntimeSnapshot(second);

    QVERIFY(!result.isOk());
    QCOMPARE(result.error().code, QStringLiteral("STATE_VERSION_CONFLICT"));
    QCOMPARE(session.runtimeSnapshot()->profileId, kProfileId);
}

void TestAgentRuntimeServices::configHash_whenSameEffectiveJsonHasDifferentKeyOrder_shouldRemainDeterministic() {
    QTemporaryDir directory;
    const QString firstPath = directory.filePath(QStringLiteral("first.json"));
    const QString secondPath = directory.filePath(QStringLiteral("second.json"));
    QVERIFY(writeFile(firstPath, R"({"b":[2,1],"a":{"y":2,"x":1}})"));
    QVERIFY(writeFile(secondPath, R"({"a":{"x":1,"y":2},"b":[2,1]})"));
    ConfigManager& config = ConfigManager::instance();
    QVERIFY(config.loadConfig(firstPath));
    const QString firstHash = config.configHash();
    QVERIFY(config.loadConfig(secondPath));

    QCOMPARE(config.configHash(), firstHash);
    QCOMPARE(firstHash.size(), 64);
}

void TestAgentRuntimeServices::identityBaselineHash_whenBaselineChanges_shouldProduceDifferentVersionHash() {
    QTemporaryDir directory;
    const QString firstPath = directory.filePath(QStringLiteral("first.json"));
    const QString secondPath = directory.filePath(QStringLiteral("second.json"));
    QVERIFY(writeFile(firstPath,
        R"({"aiSettings":{"identityBaseline":{"schemaVersion":1,"traits":{"initiative":0.4}}}})"));
    QVERIFY(writeFile(secondPath,
        R"({"aiSettings":{"identityBaseline":{"schemaVersion":1,"traits":{"initiative":0.8}}}})"));
    ConfigManager& config = ConfigManager::instance();
    QVERIFY(config.loadConfig(firstPath));
    const QString firstHash = config.identityBaselineHash();
    QVERIFY(config.loadConfig(secondPath));

    QVERIFY(config.identityBaselineHash() != firstHash);
}

void TestAgentRuntimeServices::stop_whenRuntimeIsActive_shouldRejectNewSessionsAndCloseServices() {
    StartedRuntime runtime;
    QVERIFY(runtime.ready());

    runtime.services.stop();

    QVERIFY(runtime.services.eventLedger() == nullptr);
    QVERIFY(runtime.services.eventOutbox() == nullptr);
    QVERIFY(runtime.services.unitOfWorkFactory() == nullptr);
    QVERIFY(runtime.services.captureSnapshot(QStringLiteral("late-session")).sessionId.isEmpty());
}

void TestAgentRuntimeServices::stop_whenCalledTwice_shouldRemainIdempotent() {
    StartedRuntime runtime;
    QVERIFY(runtime.ready());

    runtime.services.stop();
    runtime.services.stop();

    QVERIFY(runtime.services.eventLedger() == nullptr);
}

void TestAgentRuntimeServices::start_whenBridgeIsValid_shouldUseUiCapabilitiesWithoutTakingOwnership() {
    QTemporaryDir directory;
    AIBrain brain;
    int bubbleCalls = 0;
    int notificationCalls = 0;
    auto bridge = makeBridge(&bubbleCalls, &notificationCalls);
    RuntimeUiBridge* bridgeAddress = bridge.get();
    AgentRuntimeServices services;
    const auto result = AgentBootstrap::start(
        services, requestFor(directory, &brain, bridge.get()));
    QVERIFY(result.isOk());

    bridge->showChatBubble(QStringLiteral("hello"), 10);
    bridge->notifyUser(QStringLiteral("title"), QStringLiteral("message"), 10);

    QCOMPARE(bubbleCalls, 1);
    QCOMPARE(notificationCalls, 1);
    QCOMPARE(bridge.get(), bridgeAddress);
    QVERIFY(bridge->animationPlayer() != nullptr);
    QVERIFY(bridge->animationManager() != nullptr);
}

void TestAgentRuntimeServices::stop_whenRuntimeEnds_shouldReleaseBridgeBeforePetWindowDestroysUiObjects() {
    QTemporaryDir directory;
    AIBrain brain;
    auto bridge = makeBridge();
    AgentRuntimeServices services;
    QVERIFY(AgentBootstrap::start(
        services, requestFor(directory, &brain, bridge.get())).isOk());

    services.stop();
    bridge.reset();

    services.stop();
    QVERIFY(services.eventLedger() == nullptr);
}

QTEST_GUILESS_MAIN(TestAgentRuntimeServices)
#include "test_agent_runtime_services.moc"
