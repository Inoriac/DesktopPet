#include <QtTest>

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>

#include "ai/event/event_ledger.h"
#include "ai/event/event_outbox.h"
#include "ai/event/event_schema_registry.h"
#include "ai/event/runtime_unit_of_work.h"
#include "ai/event/sqlite_event_repository.h"

namespace {

const QString kProfileId = QStringLiteral("11111111-1111-4111-8111-111111111111");

EventDraft userMessageDraft() {
    EventDraft draft;
    draft.profileId = kProfileId;
    draft.type = QStringLiteral("UserMessageReceived");
    draft.source = QStringLiteral("test");
    draft.sessionId = QStringLiteral("session-1");
    draft.payload = {
        {QStringLiteral("text"), QStringLiteral("hello")},
        {QStringLiteral("triggerTag"), QStringLiteral("manual")}
    };
    return draft;
}

EventDraft privateDiaryDraft() {
    EventDraft draft;
    draft.profileId = kProfileId;
    draft.type = QStringLiteral("DiaryStored");
    draft.source = QStringLiteral("reflection");
    draft.privacy = EventPrivacy::Private;
    draft.privateReference = PrivateEventReference{
        QStringLiteral("private_psyche"),
        QStringLiteral("diary_entry"),
        QStringLiteral("22222222-2222-4222-8222-222222222222"),
        kProfileId
    };
    return draft;
}

void registerTestSchemas(EventSchemaRegistry& registry) {
    QVERIFY(registerBuiltInEventSchemas(registry).isOk());
    registry.freeze();
}

QString connectionName(const QString& prefix) {
    return prefix + QLatin1Char('_')
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QVariant scalar(const QString& databasePath, const QString& sql) {
    const QString name = connectionName(QStringLiteral("event_test_read"));
    QVariant value;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        database.setDatabaseName(databasePath);
        if (!database.open()) return value;
        QSqlQuery query(database);
        if (query.exec(sql) && query.next()) value = query.value(0);
        database.close();
    }
    QSqlDatabase::removeDatabase(name);
    return value;
}

bool execSql(const QString& databasePath, const QString& sql) {
    const QString name = connectionName(QStringLiteral("event_test_write"));
    bool ok = false;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        database.setDatabaseName(databasePath);
        if (database.open()) {
            QSqlQuery query(database);
            ok = query.exec(sql);
            database.close();
        }
    }
    QSqlDatabase::removeDatabase(name);
    return ok;
}

bool updateOutboxEnvelopeField(const QString& databasePath,
                               const QString& field,
                               const QString& value) {
    const QString name = connectionName(QStringLiteral("event_test_mutate"));
    bool ok = false;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        database.setDatabaseName(databasePath);
        if (database.open()) {
            QSqlQuery select(database);
            if (select.exec(QStringLiteral(
                    "SELECT outbox_id,payload_json FROM event_outbox LIMIT 1"))
                && select.next()) {
                const QString outboxId = select.value(0).toString();
                QJsonDocument document = QJsonDocument::fromJson(
                    select.value(1).toByteArray());
                QJsonObject envelope = document.object();
                envelope.insert(field, value);
                QSqlQuery update(database);
                update.prepare(QStringLiteral(
                    "UPDATE event_outbox SET payload_json=? WHERE outbox_id=?"));
                update.addBindValue(QString::fromUtf8(
                    QJsonDocument(envelope).toJson(QJsonDocument::Compact)));
                update.addBindValue(outboxId);
                ok = update.exec() && update.numRowsAffected() == 1;
            }
            database.close();
        }
    }
    QSqlDatabase::removeDatabase(name);
    return ok;
}

struct EventFixture {
    QTemporaryDir directory;
    QString databasePath;
    EventSchemaRegistry schemas;
    SqliteEventRepository repository;
    std::unique_ptr<SqliteEventLedger> ledger;
    std::unique_ptr<SqliteEventConsumerCheckpointStore> checkpoints;
    std::unique_ptr<SqliteRuntimeUnitOfWorkFactory> unitOfWorkFactory;
    std::unique_ptr<SqliteEventOutbox> outbox;

    EventFixture()
        : databasePath(directory.filePath(QStringLiteral("agent_runtime.sqlite"))) {
        registerTestSchemas(schemas);
        const Result<void, DomainError> opened = repository.open(databasePath);
        if (!opened.isOk()) return;
        ledger = std::make_unique<SqliteEventLedger>(&repository, &schemas, kProfileId);
        checkpoints = std::make_unique<SqliteEventConsumerCheckpointStore>(&repository);
        unitOfWorkFactory = std::make_unique<SqliteRuntimeUnitOfWorkFactory>(databasePath);
        outbox = std::make_unique<SqliteEventOutbox>(databasePath, &schemas, kProfileId);
    }

    bool ready() const {
        return directory.isValid() && repository.isOpen() && ledger && checkpoints
            && unitOfWorkFactory && outbox;
    }
};

} // namespace

class TestEventLedger : public QObject {
    Q_OBJECT

private slots:
    void validate_whenBuiltInSchemaMatches_shouldAcceptDraftAndPreserveUnknownFields();
    void registerSchema_whenRegistryIsFrozen_shouldRejectWithoutReplacingDefinition();
    void validate_whenPrivateDraftContainsBodyOrInvalidReference_shouldReject();

    void append_whenDraftIsValid_shouldPersistAndReturnSequencedEvent();
    void append_whenOccurredAtMissing_shouldUseUtcClock();
    void append_whenPrivateReferenceIsValid_shouldPersistReferenceWithoutBody();
    void append_whenPrivatePayloadContainsBody_shouldRejectWithoutPartialRow();
    void append_whenSchemaInvalid_shouldRejectWithoutPartialRow();

    void commit_whenExpectedSequenceMatches_shouldAdvanceCheckpoint();
    void commit_whenExpectedSequenceConflicts_shouldPreserveCurrentCheckpoint();

    void enqueue_whenDomainWriteUsesSameUnitOfWork_shouldCommitStateAndPendingEventTogether();
    void enqueue_whenOutboxWriteFails_shouldRollBackDomainState();
    void enqueue_whenUnitOfWorkIsCommittedOrRolledBack_shouldRejectWithoutOutboxWrite();

    void dispatchPending_whenDomainTransactionCommitted_shouldAppendEventAndMarkDelivered();
    void dispatchPending_whenTransactionFailsAfterInsert_shouldRollBackEventAndKeepOutboxPending();
    void dispatchPending_whenAppendTemporarilyFails_shouldKeepPendingAndAdvanceRetryMetadata();
    void dispatchPending_whenReplayEnvelopeIsCorrupted_shouldKeepPendingAndAdvanceRetryMetadata_data();
    void dispatchPending_whenReplayEnvelopeIsCorrupted_shouldKeepPendingAndAdvanceRetryMetadata();
    void dispatchPending_whenRetryMetadataUpdateFails_shouldPreserveDeliveryError();
};

void TestEventLedger::validate_whenBuiltInSchemaMatches_shouldAcceptDraftAndPreserveUnknownFields() {
    EventSchemaRegistry schemas;
    QVERIFY(registerBuiltInEventSchemas(schemas).isOk());
    EventDraft draft = userMessageDraft();
    draft.payload.insert(QStringLiteral("futureField"), QJsonObject{{QStringLiteral("v"), 1}});

    QVERIFY(schemas.validate(draft).isOk());
    QCOMPARE(draft.payload.value(QStringLiteral("futureField")).toObject()
                 .value(QStringLiteral("v")).toInt(), 1);
}

void TestEventLedger::registerSchema_whenRegistryIsFrozen_shouldRejectWithoutReplacingDefinition() {
    EventSchemaRegistry schemas;
    EventSchemaDefinition definition;
    definition.type = QStringLiteral("Example");
    definition.requiredFields.insert(QStringLiteral("name"), EventFieldType::String);
    QVERIFY(schemas.registerSchema(definition).isOk());
    schemas.freeze();

    definition.requiredFields.clear();
    QVERIFY(!schemas.registerSchema(definition).isOk());

    EventDraft invalid;
    invalid.profileId = kProfileId;
    invalid.type = QStringLiteral("Example");
    invalid.source = QStringLiteral("test");
    QVERIFY(!schemas.validate(invalid).isOk());
}

void TestEventLedger::validate_whenPrivateDraftContainsBodyOrInvalidReference_shouldReject() {
    EventSchemaRegistry schemas;
    registerTestSchemas(schemas);

    EventDraft withBody = privateDiaryDraft();
    withBody.payload.insert(QStringLiteral("body"), QStringLiteral("secret"));
    QVERIFY(!schemas.validate(withBody).isOk());

    EventDraft invalidReference = privateDiaryDraft();
    invalidReference.privateReference->store = QStringLiteral("memory");
    QVERIFY(!schemas.validate(invalidReference).isOk());
}

void TestEventLedger::append_whenDraftIsValid_shouldPersistAndReturnSequencedEvent() {
    EventFixture fixture;
    QVERIFY(fixture.ready());

    const Result<EventRecord, DomainError> result = fixture.ledger->append(userMessageDraft());
    QVERIFY(result.isOk());
    QCOMPARE(result.value().sequence, 1);
    QVERIFY(!result.value().eventId.isEmpty());
    QCOMPARE(scalar(fixture.databasePath,
                    QStringLiteral("SELECT COUNT(*) FROM event_log")).toInt(), 1);
}

void TestEventLedger::append_whenOccurredAtMissing_shouldUseUtcClock() {
    EventFixture fixture;
    QVERIFY(fixture.ready());
    const QDateTime before = QDateTime::currentDateTimeUtc();

    const Result<EventRecord, DomainError> result = fixture.ledger->append(userMessageDraft());

    QVERIFY(result.isOk());
    QVERIFY(result.value().occurredAt.isValid());
    QCOMPARE(result.value().occurredAt.timeSpec(), Qt::UTC);
    QVERIFY(result.value().occurredAt >= before);
}

void TestEventLedger::append_whenPrivateReferenceIsValid_shouldPersistReferenceWithoutBody() {
    EventFixture fixture;
    QVERIFY(fixture.ready());

    const Result<EventRecord, DomainError> result = fixture.ledger->append(privateDiaryDraft());

    QVERIFY(result.isOk());
    QVERIFY(result.value().payload.isEmpty());
    QVERIFY(result.value().privateReference.has_value());
    QCOMPARE(result.value().privateReference->recordType, QStringLiteral("diary_entry"));
    QCOMPARE(scalar(fixture.databasePath,
                    QStringLiteral("SELECT payload_json FROM event_log LIMIT 1")).toString(),
             QStringLiteral("{}"));
}

void TestEventLedger::append_whenPrivatePayloadContainsBody_shouldRejectWithoutPartialRow() {
    EventFixture fixture;
    QVERIFY(fixture.ready());
    EventDraft draft = privateDiaryDraft();
    draft.payload.insert(QStringLiteral("body"), QStringLiteral("must not persist"));

    QVERIFY(!fixture.ledger->append(draft).isOk());
    QCOMPARE(scalar(fixture.databasePath,
                    QStringLiteral("SELECT COUNT(*) FROM event_log")).toInt(), 0);
}

void TestEventLedger::append_whenSchemaInvalid_shouldRejectWithoutPartialRow() {
    EventFixture fixture;
    QVERIFY(fixture.ready());
    EventDraft draft = userMessageDraft();
    draft.payload.remove(QStringLiteral("text"));

    QVERIFY(!fixture.ledger->append(draft).isOk());
    QCOMPARE(scalar(fixture.databasePath,
                    QStringLiteral("SELECT COUNT(*) FROM event_log")).toInt(), 0);
}

void TestEventLedger::commit_whenExpectedSequenceMatches_shouldAdvanceCheckpoint() {
    EventFixture fixture;
    QVERIFY(fixture.ready());
    QCOMPARE(fixture.checkpoints->current(QStringLiteral("identity")).value(), 0);

    QVERIFY(fixture.checkpoints->commit(QStringLiteral("identity"), 0, 120).isOk());

    QCOMPARE(fixture.checkpoints->current(QStringLiteral("identity")).value(), 120);
}

void TestEventLedger::commit_whenExpectedSequenceConflicts_shouldPreserveCurrentCheckpoint() {
    EventFixture fixture;
    QVERIFY(fixture.ready());
    QVERIFY(fixture.checkpoints->commit(QStringLiteral("identity"), 0, 100).isOk());

    const Result<void, DomainError> result =
        fixture.checkpoints->commit(QStringLiteral("identity"), 0, 120);

    QVERIFY(!result.isOk());
    QCOMPARE(result.error().code, QStringLiteral("STATE_VERSION_CONFLICT"));
    QCOMPARE(fixture.checkpoints->current(QStringLiteral("identity")).value(), 100);
}

void TestEventLedger::enqueue_whenDomainWriteUsesSameUnitOfWork_shouldCommitStateAndPendingEventTogether() {
    EventFixture fixture;
    QVERIFY(fixture.ready());
    Result<std::unique_ptr<RuntimeUnitOfWork>, DomainError> begun =
        fixture.unitOfWorkFactory->begin();
    QVERIFY(begun.isOk());
    std::unique_ptr<RuntimeUnitOfWork> unitOfWork = begun.takeValue();
    QSqlDatabase database = QSqlDatabase::database(unitOfWork->connectionName());
    QSqlQuery query(database);
    QVERIFY(query.exec(QStringLiteral(
        "CREATE TABLE domain_state (id TEXT PRIMARY KEY, value TEXT NOT NULL)")));
    QVERIFY(query.exec(QStringLiteral(
        "INSERT INTO domain_state(id, value) VALUES ('state-1', 'ready')")));

    QVERIFY(fixture.outbox->enqueue(*unitOfWork, userMessageDraft()).isOk());
    QVERIFY(unitOfWork->commit().isOk());

    QCOMPARE(scalar(fixture.databasePath,
                    QStringLiteral("SELECT COUNT(*) FROM domain_state")).toInt(), 1);
    QCOMPARE(scalar(fixture.databasePath,
                    QStringLiteral("SELECT COUNT(*) FROM event_outbox WHERE status='Pending'")).toInt(), 1);
}

void TestEventLedger::enqueue_whenOutboxWriteFails_shouldRollBackDomainState() {
    EventFixture fixture;
    QVERIFY(fixture.ready());
    Result<std::unique_ptr<RuntimeUnitOfWork>, DomainError> begun =
        fixture.unitOfWorkFactory->begin();
    QVERIFY(begun.isOk());
    std::unique_ptr<RuntimeUnitOfWork> unitOfWork = begun.takeValue();
    QSqlDatabase database = QSqlDatabase::database(unitOfWork->connectionName());
    QSqlQuery query(database);
    QVERIFY(query.exec(QStringLiteral(
        "CREATE TABLE domain_state (id TEXT PRIMARY KEY, value TEXT NOT NULL)")));
    QVERIFY(query.exec(QStringLiteral(
        "INSERT INTO domain_state(id, value) VALUES ('state-1', 'ready')")));
    QVERIFY(query.exec(QStringLiteral(
        "CREATE TEMP TRIGGER reject_outbox BEFORE INSERT ON event_outbox "
        "BEGIN SELECT RAISE(ABORT, 'outbox unavailable'); END")));

    QVERIFY(!fixture.outbox->enqueue(*unitOfWork, userMessageDraft()).isOk());
    unitOfWork->rollback();

    QCOMPARE(scalar(fixture.databasePath,
                    QStringLiteral("SELECT COUNT(*) FROM domain_state")).toInt(), 0);
    QCOMPARE(scalar(fixture.databasePath,
                    QStringLiteral("SELECT COUNT(*) FROM event_outbox")).toInt(), 0);
}

void TestEventLedger::enqueue_whenUnitOfWorkIsCommittedOrRolledBack_shouldRejectWithoutOutboxWrite() {
    EventFixture fixture;
    QVERIFY(fixture.ready());

    auto committedResult = fixture.unitOfWorkFactory->begin();
    QVERIFY(committedResult.isOk());
    std::unique_ptr<RuntimeUnitOfWork> committed = committedResult.takeValue();
    QVERIFY(committed->commit().isOk());
    const auto enqueueAfterCommit = fixture.outbox->enqueue(*committed, userMessageDraft());

    auto rolledBackResult = fixture.unitOfWorkFactory->begin();
    QVERIFY(rolledBackResult.isOk());
    std::unique_ptr<RuntimeUnitOfWork> rolledBack = rolledBackResult.takeValue();
    rolledBack->rollback();
    const auto enqueueAfterRollback = fixture.outbox->enqueue(*rolledBack, userMessageDraft());

    QVERIFY(!enqueueAfterCommit.isOk());
    QVERIFY(!enqueueAfterRollback.isOk());
    QCOMPARE(scalar(fixture.databasePath,
                    QStringLiteral("SELECT COUNT(*) FROM event_outbox")).toInt(), 0);
}

void TestEventLedger::dispatchPending_whenDomainTransactionCommitted_shouldAppendEventAndMarkDelivered() {
    EventFixture fixture;
    QVERIFY(fixture.ready());
    auto begun = fixture.unitOfWorkFactory->begin();
    QVERIFY(begun.isOk());
    std::unique_ptr<RuntimeUnitOfWork> unitOfWork = begun.takeValue();
    QVERIFY(fixture.outbox->enqueue(*unitOfWork, userMessageDraft()).isOk());
    QVERIFY(unitOfWork->commit().isOk());

    const Result<int, DomainError> dispatched = fixture.outbox->dispatchPending(10);

    QVERIFY(dispatched.isOk());
    QCOMPARE(dispatched.value(), 1);
    QCOMPARE(scalar(fixture.databasePath,
                    QStringLiteral("SELECT COUNT(*) FROM event_log")).toInt(), 1);
    QCOMPARE(scalar(fixture.databasePath,
                    QStringLiteral("SELECT status FROM event_outbox")).toString(),
             QStringLiteral("Delivered"));
}

void TestEventLedger::dispatchPending_whenTransactionFailsAfterInsert_shouldRollBackEventAndKeepOutboxPending() {
    EventFixture fixture;
    QVERIFY(fixture.ready());
    auto begun = fixture.unitOfWorkFactory->begin();
    QVERIFY(begun.isOk());
    std::unique_ptr<RuntimeUnitOfWork> unitOfWork = begun.takeValue();
    QVERIFY(fixture.outbox->enqueue(*unitOfWork, userMessageDraft()).isOk());
    QVERIFY(unitOfWork->commit().isOk());
    QVERIFY(execSql(fixture.databasePath, QStringLiteral(
        "CREATE TRIGGER reject_delivery BEFORE UPDATE OF status ON event_outbox "
        "WHEN NEW.status='Delivered' BEGIN SELECT RAISE(ABORT, 'delivery failed'); END")));

    QVERIFY(!fixture.outbox->dispatchPending(10).isOk());

    QCOMPARE(scalar(fixture.databasePath,
                    QStringLiteral("SELECT COUNT(*) FROM event_log")).toInt(), 0);
    QCOMPARE(scalar(fixture.databasePath,
                    QStringLiteral("SELECT status FROM event_outbox")).toString(),
             QStringLiteral("Pending"));
}

void TestEventLedger::dispatchPending_whenAppendTemporarilyFails_shouldKeepPendingAndAdvanceRetryMetadata() {
    EventFixture fixture;
    QVERIFY(fixture.ready());
    auto begun = fixture.unitOfWorkFactory->begin();
    QVERIFY(begun.isOk());
    std::unique_ptr<RuntimeUnitOfWork> unitOfWork = begun.takeValue();
    QVERIFY(fixture.outbox->enqueue(*unitOfWork, userMessageDraft()).isOk());
    QVERIFY(unitOfWork->commit().isOk());
    QVERIFY(execSql(fixture.databasePath,
                    QStringLiteral("UPDATE event_outbox SET payload_json='{invalid json}'")));

    QVERIFY(!fixture.outbox->dispatchPending(10).isOk());

    QCOMPARE(scalar(fixture.databasePath,
                    QStringLiteral("SELECT status FROM event_outbox")).toString(),
             QStringLiteral("Pending"));
    QCOMPARE(scalar(fixture.databasePath,
                    QStringLiteral("SELECT attempt_count FROM event_outbox")).toInt(), 1);
    QVERIFY(!scalar(fixture.databasePath,
                    QStringLiteral("SELECT next_attempt_at FROM event_outbox")).toString().isEmpty());
}

void TestEventLedger::dispatchPending_whenReplayEnvelopeIsCorrupted_shouldKeepPendingAndAdvanceRetryMetadata_data() {
    QTest::addColumn<QString>("field");
    QTest::addColumn<QString>("value");

    QTest::newRow("empty event id") << QStringLiteral("eventId") << QString();
    QTest::newRow("invalid occurred-at")
        << QStringLiteral("occurredAt") << QStringLiteral("not-a-date");
    QTest::newRow("non-UTC occurred-at")
        << QStringLiteral("occurredAt") << QStringLiteral("2026-08-24T12:00:00.000+08:00");
}

void TestEventLedger::dispatchPending_whenReplayEnvelopeIsCorrupted_shouldKeepPendingAndAdvanceRetryMetadata() {
    QFETCH(QString, field);
    QFETCH(QString, value);
    EventFixture fixture;
    QVERIFY(fixture.ready());
    auto begun = fixture.unitOfWorkFactory->begin();
    QVERIFY(begun.isOk());
    std::unique_ptr<RuntimeUnitOfWork> unitOfWork = begun.takeValue();
    QVERIFY(fixture.outbox->enqueue(*unitOfWork, userMessageDraft()).isOk());
    QVERIFY(unitOfWork->commit().isOk());
    QVERIFY(updateOutboxEnvelopeField(fixture.databasePath, field, value));

    const auto dispatched = fixture.outbox->dispatchPending(10);

    QVERIFY(!dispatched.isOk());
    QCOMPARE(dispatched.error().code, QStringLiteral("EVT_SCHEMA_INVALID"));
    QCOMPARE(scalar(fixture.databasePath,
                    QStringLiteral("SELECT COUNT(*) FROM event_log")).toInt(), 0);
    QCOMPARE(scalar(fixture.databasePath,
                    QStringLiteral("SELECT status FROM event_outbox")).toString(),
             QStringLiteral("Pending"));
    QCOMPARE(scalar(fixture.databasePath,
                    QStringLiteral("SELECT attempt_count FROM event_outbox")).toInt(), 1);
    QVERIFY(!scalar(fixture.databasePath,
                    QStringLiteral("SELECT next_attempt_at FROM event_outbox")).toString().isEmpty());
}

void TestEventLedger::dispatchPending_whenRetryMetadataUpdateFails_shouldPreserveDeliveryError() {
    EventFixture fixture;
    QVERIFY(fixture.ready());
    auto begun = fixture.unitOfWorkFactory->begin();
    QVERIFY(begun.isOk());
    std::unique_ptr<RuntimeUnitOfWork> unitOfWork = begun.takeValue();
    QVERIFY(fixture.outbox->enqueue(*unitOfWork, userMessageDraft()).isOk());
    QVERIFY(unitOfWork->commit().isOk());
    QVERIFY(execSql(fixture.databasePath,
                    QStringLiteral("UPDATE event_outbox SET payload_json='{invalid json}'")));
    QVERIFY(execSql(fixture.databasePath, QStringLiteral(
        "CREATE TRIGGER reject_retry BEFORE UPDATE OF attempt_count ON event_outbox "
        "BEGIN SELECT RAISE(ABORT, 'retry metadata failed'); END")));

    const auto dispatched = fixture.outbox->dispatchPending(10);

    QVERIFY(!dispatched.isOk());
    QCOMPARE(dispatched.error().code, QStringLiteral("EVENT_OUTBOX_UNAVAILABLE"));
    QVERIFY(dispatched.error().message.contains(
        QStringLiteral("outbox payload is invalid JSON")));
    QCOMPARE(dispatched.error().details.value(
                 QStringLiteral("retryMetadataAdvanced")).toBool(), false);
    QVERIFY(!dispatched.error().details.value(
                 QStringLiteral("retryMetadataError")).toString().isEmpty());
    QCOMPARE(scalar(fixture.databasePath,
                    QStringLiteral("SELECT attempt_count FROM event_outbox")).toInt(), 0);
    QVERIFY(scalar(fixture.databasePath,
                   QStringLiteral("SELECT next_attempt_at FROM event_outbox")).isNull());
}

QTEST_APPLESS_MAIN(TestEventLedger)
#include "test_event_ledger.moc"
