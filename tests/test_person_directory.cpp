#include <QtTest>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <barrier>
#include <future>

#include "ai/event/event_ledger.h"
#include "ai/event/event_outbox.h"
#include "ai/event/event_schema_registry.h"
#include "ai/event/sqlite_event_repository.h"
#include "ai/identity/person_directory.h"
#include "ai/identity/relationship_service.h"
#include "ai/identity/sqlite_identity_repository.h"

namespace {
const QString profile = QStringLiteral("11111111-1111-4111-8111-111111111111");
const QString otherProfile = QStringLiteral("33333333-3333-4333-8333-333333333333");
ExternalIdentity account(const QString& id, const QString& name = QStringLiteral("小明")) {
    return {QStringLiteral("chat"), QStringLiteral("global"), id, name};
}
ResolvePersonRequest request(const QString& id, const QString& name = QStringLiteral("小明")) {
    return {profile, account(id, name), false};
}
QVariant sql(const QString& path, const QString& statement) {
    const auto connection = QUuid::createUuid().toString();
    QVariant result;
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(path);
        if (db.open()) {
            QSqlQuery q(db);
            if (q.exec(statement)) result = q.next() ? q.value(0) : QVariant(true);
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return result;
}
struct Fixture {
    QTemporaryDir dir;
    QString path = dir.filePath(QStringLiteral("runtime.sqlite"));
    SqliteEventRepository events;
    EventSchemaRegistry schemas;
    SqliteIdentityRepository identities;
    SqliteRuntimeUnitOfWorkFactory factory{path};
    SqliteEventOutbox outbox{path, &schemas, profile};
    PersonDirectory people{profile, &identities, &factory, &outbox};
    bool ready() {
        return events.open(path).isOk() && identities.open(path).isOk()
            && registerBuiltInEventSchemas(schemas).isOk();
    }
    QString evidence(const ExternalIdentity& external, const QString& person, qint64 revision) {
        SqliteEventLedger ledger(&events, &schemas, profile);
        EventDraft event;
        event.profileId = profile;
        event.type = QStringLiteral("IdentityBindingAccepted");
        event.source = QStringLiteral("IdentityManagement");
        event.payload = {{"provider", external.provider}, {"namespace", external.nameSpace},
            {"remote_actor_id", external.remoteActorId}, {"person_id", person},
            {"expected_revision", revision}};
        const auto stored = ledger.append(event);
        return stored.isOk() ? stored.value().eventId : QString();
    }
};
}

class PersonDirectoryTests : public QObject {
    Q_OBJECT
private slots:
    void resolvePerson_sameAccountRenamed_preservesPersonId();
    void resolvePerson_sameDisplayName_distinctPeopleAndRelationships();
    void resolvePerson_sameAccountDifferentProfile_isolated();
    void resolvePerson_missingStableId_unresolvedWithoutSharedPerson();
    void resolvePerson_trustedLocalOwner_preservesOwnerHistory();
    void resolvePerson_concurrentFirstResolution_onePersonAndMapping();
    void resolvePerson_storeFailure_noOwnerFallbackOrPartialPerson();
    void bindIdentity_newAccountWithEvidence_reusesPersonAndHistory();
    void bindIdentity_conflictingAccount_preservesOriginalMapping();
    void bindIdentity_staleRevisionOrMissingEvidence_rejectsWithoutMutation();
    void bindIdentity_outboxFailure_rollsBackMappingAndRevision();
    void bindIdentity_evidenceMustMatchHostAndScope_data();
    void bindIdentity_evidenceMustMatchHostAndScope();
    void initialize_failureRollsBackOwnerAndPreservesHistory();
    void migration_existingRelationshipSubjects_preservesIdsAndVersions();
};

void PersonDirectoryTests::resolvePerson_sameAccountRenamed_preservesPersonId() {
    Fixture f; QVERIFY(f.ready());
    auto first = f.people.resolvePerson(request("100")); QVERIFY(first.isOk());
    QCOMPARE(first.value().status, PersonResolutionStatus::Provisional);
    QVERIFY(first.value().personId.has_value());
    QVERIFY(!QUuid(*first.value().personId).isNull());
    auto renamed = f.people.resolvePerson(request("100", "新名字")); QVERIFY(renamed.isOk());
    QCOMPARE(renamed.value().personId, first.value().personId);
    QCOMPARE(renamed.value().identityRevision, first.value().identityRevision);
    QCOMPARE(sql(f.path, "SELECT display_name FROM agent_external_identities").toString(), QString("新名字"));
}
void PersonDirectoryTests::resolvePerson_sameDisplayName_distinctPeopleAndRelationships() {
    Fixture f; QVERIFY(f.ready());
    auto a = f.people.resolvePerson(request("100")); auto b = f.people.resolvePerson(request("101"));
    QVERIFY(a.isOk() && b.isOk()); QVERIFY(a.value().personId != b.value().personId);
    RelationshipService relations(profile, &f.identities);
    QVERIFY(relations.applyEvidence(*a.value().personId,
        {"trust", 1.0, 1.0, 1.0, "first-interaction", QDateTime::currentDateTimeUtc()}).isOk());
    QVERIFY(f.identities.currentRelationship(profile, *a.value().personId).value().has_value());
    QVERIFY(!f.identities.currentRelationship(profile, *b.value().personId).value().has_value());
}
void PersonDirectoryTests::resolvePerson_sameAccountDifferentProfile_isolated() {
    Fixture f; QVERIFY(f.ready());
    PersonDirectory other(otherProfile, &f.identities, &f.factory, &f.outbox);
    auto r = request("100"); auto a = f.people.resolvePerson(r); r.profileId = otherProfile;
    auto b = other.resolvePerson(r); QVERIFY(a.isOk() && b.isOk());
    QVERIFY(a.value().personId != b.value().personId);
    auto rejected = f.people.resolvePerson(r); QVERIFY(!rejected.isOk());
    QCOMPARE(rejected.error().code, QString("INVALID_INPUT"));
    r = request("100"); r.externalIdentity->nameSpace = "other-scope";
    auto c = f.people.resolvePerson(r); QVERIFY(c.isOk()); QVERIFY(c.value().personId != a.value().personId);
}
void PersonDirectoryTests::resolvePerson_missingStableId_unresolvedWithoutSharedPerson() {
    Fixture f; QVERIFY(f.ready());
    for (const auto& name : {QString("小明"), QString("owner")}) {
        auto r = f.people.resolvePerson(request("", name)); QVERIFY(r.isOk());
        QCOMPARE(r.value().status, PersonResolutionStatus::Unresolved);
        QVERIFY(!r.value().personId.has_value()); QCOMPARE(r.value().identityRevision, 0);
        QCOMPARE(r.value().externalIdentity->displayName, name);
    }
    QCOMPARE(sql(f.path, "SELECT count(*) FROM agent_persons").toInt(), 0);
    auto invalid = request("100"); invalid.externalIdentity->provider.clear();
    QVERIFY(!f.people.resolvePerson(invalid).isOk());
    invalid = request("100"); invalid.ownerLocal = true;
    QVERIFY(!f.people.resolvePerson(invalid).isOk());
}
void PersonDirectoryTests::resolvePerson_trustedLocalOwner_preservesOwnerHistory() {
    Fixture f; QVERIFY(f.ready());
    RelationshipService relations(profile, &f.identities);
    auto old = relations.applyEvidence("owner", {"trust", 1.0, 1.0, 1.0, "old-owner", QDateTime::currentDateTimeUtc()});
    QVERIFY(old.isOk());
    auto owner = f.people.resolvePerson({profile, std::nullopt, true}); QVERIFY(owner.isOk());
    QCOMPARE(owner.value().personId, std::optional<QString>("owner"));
    QCOMPARE(owner.value().status, PersonResolutionStatus::Resolved);
    auto after = f.identities.currentRelationship(profile, "owner"); QVERIFY(after.isOk());
    QCOMPARE(after.value()->stateId, old.value().stateId);
    QCOMPARE(after.value()->version, old.value().version);
    const auto externalOwner = account("owner-account");
    const auto evidence = f.evidence(externalOwner, "owner", owner.value().identityRevision);
    QVERIFY(f.people.bindIdentity({profile, externalOwner, "owner", owner.value().identityRevision, evidence}).isOk());
    const auto resolvedOwner = f.people.resolvePerson(request("owner-account"));
    QVERIFY(resolvedOwner.isOk()); QCOMPARE(resolvedOwner.value().personId, owner.value().personId);
    QCOMPARE(f.identities.currentRelationship(profile, "owner").value()->stateId, old.value().stateId);
}
void PersonDirectoryTests::resolvePerson_concurrentFirstResolution_onePersonAndMapping() {
    Fixture f; QVERIFY(f.ready());
    std::barrier start(2);
    auto resolve = [&]() {
        SqliteIdentityRepository repo;
        const auto opened = repo.open(f.path);
        SqliteRuntimeUnitOfWorkFactory factory(f.path);
        PersonDirectory directory(profile, &repo, &factory, nullptr);
        start.arrive_and_wait();
        if (!opened.isOk()) return QString();
        auto result = directory.resolvePerson(request("concurrent"));
        return result.isOk() ? result.value().personId.value_or(QString()) : QString();
    };
    auto a = std::async(std::launch::async, resolve); auto b = std::async(std::launch::async, resolve);
    const QString first = a.get(); const QString second = b.get();
    QVERIFY(!first.isEmpty()); QCOMPARE(first, second);
    QCOMPARE(sql(f.path, "SELECT count(*) FROM agent_persons WHERE kind='person'").toInt(), 1);
    QCOMPARE(sql(f.path, "SELECT count(*) FROM agent_external_identities").toInt(), 1);
}
void PersonDirectoryTests::resolvePerson_storeFailure_noOwnerFallbackOrPartialPerson() {
    Fixture f; QVERIFY(f.ready());
    QVERIFY(sql(f.path, "CREATE TRIGGER reject_identity BEFORE INSERT ON agent_external_identities BEGIN SELECT RAISE(ABORT,'injected failure'); END").isValid());
    auto failed = f.people.resolvePerson(request("100")); QVERIFY(!failed.isOk());
    QCOMPARE(failed.error().code, QString("STORE_UNAVAILABLE"));
    QCOMPARE(sql(f.path, "SELECT count(*) FROM agent_persons").toInt(), 0);
    f.identities.close(); failed = f.people.resolvePerson(request("100")); QVERIFY(!failed.isOk());
    QCOMPARE(failed.error().code, QString("STORE_UNAVAILABLE"));
}
void PersonDirectoryTests::bindIdentity_newAccountWithEvidence_reusesPersonAndHistory() {
    Fixture f; QVERIFY(f.ready());
    auto a = f.people.resolvePerson(request("100")); QVERIFY(a.isOk());
    const QString person = *a.value().personId;
    RelationshipService relations(profile, &f.identities);
    auto old = relations.applyEvidence(person, {"trust", 1.0, 1.0, 1.0, "old-friend", QDateTime::currentDateTimeUtc()});
    QVERIFY(old.isOk());
    auto second = account("200"); const QString eventId = f.evidence(second, person, a.value().identityRevision);
    QVERIFY(!eventId.isEmpty());
    BindIdentityRequest binding{profile, second, person, a.value().identityRevision, eventId};
    auto bound = f.people.bindIdentity(binding); QVERIFY2(bound.isOk(), qPrintable(bound.isOk() ? QString() : bound.error().message));
    QVERIFY(f.people.bindIdentity(binding).isOk());
    auto b = f.people.resolvePerson(request("200")); QVERIFY(b.isOk()); QCOMPARE(b.value().personId, a.value().personId);
    QCOMPARE(b.value().identityRevision, a.value().identityRevision + 1);
    QCOMPARE(f.people.resolvePerson(request("100")).value().identityRevision, b.value().identityRevision);
    QCOMPARE(f.identities.currentRelationship(profile, person).value()->stateId, old.value().stateId);
    const auto third = account("300");
    auto stale = f.people.bindIdentity({profile, third, person, a.value().identityRevision,
        f.evidence(third, person, a.value().identityRevision)});
    QVERIFY(!stale.isOk()); QCOMPARE(stale.error().code, QStringLiteral("STALE_REVISION"));
    QCOMPARE(sql(f.path, "SELECT count(*) FROM agent_external_identities").toInt(), 2);
    QCOMPARE(sql(f.path, "SELECT count(*) FROM event_outbox").toInt(), 1);
    QCOMPARE(sql(f.path, "SELECT binding_evidence_ref FROM agent_external_identities WHERE remote_actor_id='200'").toString(), eventId);
}
void PersonDirectoryTests::bindIdentity_conflictingAccount_preservesOriginalMapping() {
    Fixture f; QVERIFY(f.ready());
    auto a = f.people.resolvePerson(request("100")); auto b = f.people.resolvePerson(request("200")); QVERIFY(a.isOk() && b.isOk());
    BindIdentityRequest bind{profile, account("200"), *a.value().personId, a.value().identityRevision, ""};
    auto result = f.people.bindIdentity(bind); QVERIFY(!result.isOk()); QCOMPARE(result.error().code, QString("IDENTITY_CONFLICT"));
    QCOMPARE(f.people.resolvePerson(request("200")).value().personId, b.value().personId);
}
void PersonDirectoryTests::bindIdentity_staleRevisionOrMissingEvidence_rejectsWithoutMutation() {
    Fixture f; QVERIFY(f.ready()); auto a = f.people.resolvePerson(request("100")); QVERIFY(a.isOk());
    BindIdentityRequest bind{profile, account("200"), *a.value().personId, a.value().identityRevision, ""};
    auto missing = f.people.bindIdentity(bind); QVERIFY(!missing.isOk()); QCOMPARE(missing.error().code, QString("INVALID_INPUT"));
    bind.bindingEvidenceRef = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto unknown = f.people.bindIdentity(bind); QVERIFY(!unknown.isOk()); QCOMPARE(unknown.error().code, QString("UNKNOWN_REFERENCE"));
    bind.bindingEvidenceRef = f.evidence(bind.externalIdentity, bind.personId, bind.expectedRevision + 1);
    auto mismatch = f.people.bindIdentity(bind); QVERIFY(!mismatch.isOk()); QCOMPARE(mismatch.error().code, QString("UNKNOWN_REFERENCE"));
    bind.expectedRevision += 1;
    auto stale = f.people.bindIdentity(bind); QVERIFY(!stale.isOk()); QCOMPARE(stale.error().code, QString("STALE_REVISION"));
    const auto unknownAccount = account("300");
    auto missingPerson = f.people.bindIdentity({profile, unknownAccount, "missing-person", 1,
        f.evidence(unknownAccount, "missing-person", 1)});
    QVERIFY(!missingPerson.isOk()); QCOMPARE(missingPerson.error().code, QStringLiteral("UNKNOWN_REFERENCE"));
    QCOMPARE(sql(f.path, "SELECT count(*) FROM agent_external_identities").toInt(), 1);
    QCOMPARE(f.people.resolvePerson(request("100")).value().identityRevision, a.value().identityRevision);
}
void PersonDirectoryTests::bindIdentity_outboxFailure_rollsBackMappingAndRevision() {
    Fixture f; QVERIFY(f.ready()); auto a = f.people.resolvePerson(request("100")); QVERIFY(a.isOk());
    const auto external = account("200");
    BindIdentityRequest bind{profile, external, *a.value().personId, a.value().identityRevision,
        f.evidence(external, *a.value().personId, a.value().identityRevision)};
    QVERIFY(sql(f.path, "CREATE TRIGGER reject_outbox BEFORE INSERT ON event_outbox BEGIN SELECT RAISE(ABORT,'injected failure'); END").isValid());
    auto result = f.people.bindIdentity(bind); QVERIFY(!result.isOk()); QCOMPARE(result.error().code, QString("STORE_UNAVAILABLE"));
    QCOMPARE(sql(f.path, "SELECT count(*) FROM agent_external_identities").toInt(), 1);
    QCOMPARE(f.people.resolvePerson(request("100")).value().identityRevision, a.value().identityRevision);
}
void PersonDirectoryTests::bindIdentity_evidenceMustMatchHostAndScope_data() {
    QTest::addColumn<QString>("changedField");
    for (const auto& field : {"source", "type", "profile", "person_id", "provider", "namespace", "remote_actor_id"})
        QTest::newRow(field) << QString::fromLatin1(field);
}
void PersonDirectoryTests::bindIdentity_evidenceMustMatchHostAndScope() {
    QFETCH(QString, changedField);
    Fixture f; QVERIFY(f.ready()); auto a = f.people.resolvePerson(request("100")); QVERIFY(a.isOk());
    const QString person = *a.value().personId;
    EventDraft event; event.profileId = profile; event.type = QStringLiteral("IdentityBindingAccepted");
    event.source = QStringLiteral("IdentityManagement");
    event.payload = {{"provider", "chat"}, {"namespace", "global"}, {"remote_actor_id", "200"},
        {"person_id", person}, {"expected_revision", a.value().identityRevision}};
    if (changedField == QLatin1String("source")) event.source = QStringLiteral("ChatAdapter");
    else if (changedField == QLatin1String("profile")) event.profileId = otherProfile;
    else if (changedField == QLatin1String("type")) {
        event.type = QStringLiteral("UserMessageReceived");
        event.payload.insert(QStringLiteral("text"), QStringLiteral("these accounts are mine"));
        event.payload.insert(QStringLiteral("triggerTag"), QStringLiteral("manual"));
    } else event.payload.insert(changedField, QStringLiteral("wrong"));
    SqliteEventLedger ledger(&f.events, &f.schemas, event.profileId);
    auto accepted = ledger.append(event); QVERIFY(accepted.isOk());
    auto result = f.people.bindIdentity({profile, account("200"), person,
        a.value().identityRevision, accepted.value().eventId});
    QVERIFY(!result.isOk()); QCOMPARE(result.error().code, QStringLiteral("UNKNOWN_REFERENCE"));
    QCOMPARE(sql(f.path, "SELECT count(*) FROM agent_external_identities").toInt(), 1);
}
void PersonDirectoryTests::initialize_failureRollsBackOwnerAndPreservesHistory() {
    Fixture f; QVERIFY(f.ready());
    RelationshipService relations(profile, &f.identities);
    auto old = relations.applyEvidence("legacy", {"trust", 1.0, 1.0, 1.0, "legacy-evidence", QDateTime::currentDateTimeUtc()});
    QVERIFY(old.isOk());
    QVERIFY(sql(f.path, "CREATE TRIGGER reject_person BEFORE INSERT ON agent_persons WHEN NEW.person_id='legacy' BEGIN SELECT RAISE(ABORT,'injected migration failure'); END").isValid());
    auto result = f.people.initialize(); QVERIFY(!result.isOk()); QCOMPARE(result.error().code, QStringLiteral("STORE_UNAVAILABLE"));
    QCOMPARE(sql(f.path, "SELECT count(*) FROM agent_persons").toInt(), 0);
    QCOMPARE(f.identities.currentRelationship(profile, "legacy").value()->stateId, old.value().stateId);
    QVERIFY(sql(f.path, "DROP TRIGGER reject_person").isValid());
    QVERIFY(f.people.initialize().isOk()); QCOMPARE(sql(f.path, "SELECT count(*) FROM agent_persons").toInt(), 2);
}

void PersonDirectoryTests::migration_existingRelationshipSubjects_preservesIdsAndVersions() {
    Fixture f; QVERIFY(f.ready());
    RelationshipService relations(profile, &f.identities);
    for (const auto& subject : {QString("owner"), QString("legacy-friend")})
        QVERIFY(relations.applyEvidence(subject, {"trust", 1.0, 1.0, 1.0, subject, QDateTime::currentDateTimeUtc()}).isOk());
    auto old = f.identities.currentRelationship(profile, "legacy-friend").value();
    QVERIFY(sql(f.path, "DROP TABLE agent_external_identities").isValid());
    QVERIFY(sql(f.path, "DROP TABLE agent_persons").isValid());
    // Simulate an old database; a current v2 database must not rerun migration DDL.
    QVERIFY(sql(f.path, "PRAGMA user_version=1").isValid());
    QVERIFY(f.events.open(f.path).isOk()); QVERIFY(f.events.open(f.path).isOk());
    QVERIFY(f.people.initialize().isOk()); QVERIFY(f.people.initialize().isOk());
    QCOMPARE(sql(f.path, "SELECT count(*) FROM agent_persons").toInt(), 2);
    QCOMPARE(sql(f.path, "SELECT status FROM agent_persons WHERE person_id='legacy-friend'").toString(), QString("provisional"));
    QCOMPARE(sql(f.path, "SELECT kind FROM agent_persons WHERE person_id='owner'").toString(), QString("owner"));
    auto now = f.identities.currentRelationship(profile, "legacy-friend").value();
    QCOMPARE(now->stateId, old->stateId); QCOMPARE(now->version, old->version);
}
QTEST_GUILESS_MAIN(PersonDirectoryTests)
#include "test_person_directory.moc"
