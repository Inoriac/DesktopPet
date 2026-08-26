#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

#include "runtime/profile_data_migrator.h"
#include "runtime/profile_resolver.h"
#include "entity/pet.h"

class TestProfileDataMigrator : public QObject {
    Q_OBJECT

private slots:
    void resolve_whenPetAndProfileIdMatch_shouldReturnProfileWithoutOpeningStores();
    void resolve_whenPetAndProfileIdDoNotMatch_shouldRejectBeforeOpeningStores();
    void resolve_whenProfileIdArgumentIsMissing_shouldResolveItFromPetRegistry();
    void resolve_whenLegacyRegistryHasNoProfileId_shouldRequireLauncherUpgrade();
    void resolve_whenRegistryProfileFieldsAreInvalid_shouldRejectEntireMapping();
    void load_whenAnyRegistryEntryIsMalformed_shouldRejectEntireRegistry();
    void migrateLegacyMemory_whenSingleProfileAndValidLegacyDb_shouldCopyVerifyAndAtomicallyActivate();
    void migrateLegacyMemory_whenMultipleProfilesAreAmbiguous_shouldPreserveLegacyDbAndRequireSelection();
    void migrateLegacyMemory_whenIntegrityCheckFails_shouldRemoveTempCopyAndKeepLegacyPath();
    void migrateLegacyMemory_whenTargetAlreadyCommitted_shouldRemainIdempotent();
    void migrateLegacyMemory_whenOnlyLegacyJsonExists_shouldImportIntoProfileDatabase();
    void migrateLegacyMemory_whenRegisteredProfilesAreInvalid_shouldRejectRequest();

private:
    static const QString kProfileId;
    static void createLegacyDatabase(const QString& path, int rowCount = 2);
    static ProfileMigrationRequest requestFor(const QTemporaryDir& directory);
};

const QString TestProfileDataMigrator::kProfileId =
    QStringLiteral("5bb00e6d-937a-4f46-9c87-e3933c078f5a");

void TestProfileDataMigrator::createLegacyDatabase(const QString& path, int rowCount) {
    const QString connectionName = QStringLiteral("profile_migration_test_%1")
                                       .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        database.setDatabaseName(path);
        QVERIFY(database.open());
        QSqlQuery query(database);
        QVERIFY(query.exec(QStringLiteral(
            "CREATE TABLE memory_items (id TEXT PRIMARY KEY, summary TEXT)")));
        for (int i = 0; i < rowCount; ++i) {
            query.prepare(QStringLiteral(
                "INSERT INTO memory_items(id, summary) VALUES(:id, :summary)"));
            query.bindValue(QStringLiteral(":id"), QStringLiteral("memory-%1").arg(i));
            query.bindValue(QStringLiteral(":summary"), QStringLiteral("summary-%1").arg(i));
            QVERIFY(query.exec());
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(connectionName);
}

ProfileMigrationRequest TestProfileDataMigrator::requestFor(const QTemporaryDir& directory) {
    ProfileMigrationRequest request;
    request.profileId = kProfileId;
    request.registeredProfileIds = {kProfileId};
    request.appDataRoot = directory.filePath(QStringLiteral("app-data"));
    request.legacyDatabasePath = directory.filePath(QStringLiteral("runtime/memory/memory.db"));
    request.legacyJsonPath = directory.filePath(QStringLiteral("log/ai_memory.json"));
    return request;
}

void TestProfileDataMigrator::resolve_whenPetAndProfileIdMatch_shouldReturnProfileWithoutOpeningStores() {
    const PetProfile expected{
        QStringLiteral("Milltina"), QStringLiteral("models/milltina.gltf"), kProfileId};
    const ProfileResolver resolver({expected});

    const ProfileResolutionResult result = resolver.resolve(
        expected.name, std::optional<QString>{expected.profileId});

    QCOMPARE(result.status, ProfileResolutionStatus::Resolved);
    QVERIFY(result.isResolved());
    QVERIFY(result.profile.has_value());
    QCOMPARE(result.profile->profileId, expected.profileId);
    QCOMPARE(result.profile->modelPath, expected.modelPath);
}

void TestProfileDataMigrator::resolve_whenPetAndProfileIdDoNotMatch_shouldRejectBeforeOpeningStores() {
    const ProfileResolver resolver({
        {QStringLiteral("Milltina"), QStringLiteral("one.gltf"), kProfileId},
        {QStringLiteral("Nora"), QStringLiteral("two.gltf"),
         QStringLiteral("f8685597-fc48-4df7-a15a-8ccfde643c52")},
    });

    const ProfileResolutionResult result = resolver.resolve(
        QStringLiteral("Milltina"),
        std::optional<QString>{QStringLiteral("f8685597-fc48-4df7-a15a-8ccfde643c52")});

    QCOMPARE(result.status, ProfileResolutionStatus::ProfileMismatch);
    QVERIFY(!result.profile.has_value());
}

void TestProfileDataMigrator::resolve_whenProfileIdArgumentIsMissing_shouldResolveItFromPetRegistry() {
    const PetProfile expected{
        QStringLiteral("Milltina"), QStringLiteral("models/milltina.gltf"), kProfileId};
    const ProfileResolver resolver({expected});

    const ProfileResolutionResult result = resolver.resolve(expected.name, std::nullopt);

    QCOMPARE(result.status, ProfileResolutionStatus::Resolved);
    QVERIFY(result.profile.has_value());
    QCOMPARE(result.profile->profileId, kProfileId);
}

void TestProfileDataMigrator::resolve_whenLegacyRegistryHasNoProfileId_shouldRequireLauncherUpgrade() {
    const ProfileResolver resolver({
        {QStringLiteral("Milltina"), QStringLiteral("models/milltina.gltf"), QString()},
    });

    const ProfileResolutionResult result = resolver.resolve(QStringLiteral("Milltina"), std::nullopt);

    QCOMPARE(result.status, ProfileResolutionStatus::RegistryUpgradeRequired);
    QVERIFY(!result.profile.has_value());
}

void TestProfileDataMigrator::resolve_whenRegistryProfileFieldsAreInvalid_shouldRejectEntireMapping() {
    const ProfileResolver resolver({
        {QStringLiteral("Milltina"), QStringLiteral("models/milltina.gltf"), kProfileId},
        {QStringLiteral("Nora"), QString(),
         QStringLiteral("f8685597-fc48-4df7-a15a-8ccfde643c52")},
    });

    const ProfileResolutionResult result = resolver.resolve(
        QStringLiteral("Milltina"), std::optional<QString>{kProfileId});

    QCOMPARE(result.status, ProfileResolutionStatus::InvalidProfileId);
    QVERIFY(!result.profile.has_value());
}

void TestProfileDataMigrator::load_whenAnyRegistryEntryIsMalformed_shouldRejectEntireRegistry() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QJsonArray entries{
        QJsonObject{{QStringLiteral("name"), QStringLiteral("Milltina")},
                    {QStringLiteral("modelPath"), QStringLiteral("one.gltf")},
                    {QStringLiteral("profileId"), kProfileId}},
        QJsonObject{{QStringLiteral("name"), QStringLiteral("Nora")},
                    {QStringLiteral("profileId"),
                     QStringLiteral("f8685597-fc48-4df7-a15a-8ccfde643c52")}},
    };
    QFile registry(directory.filePath(QStringLiteral("pets.json")));
    QVERIFY(registry.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(registry.write(QJsonDocument(entries).toJson()) > 0);
    registry.close();

    QString errorMessage;
    QVERIFY(!Pet::instance().loadFromPath(registry.fileName(), &errorMessage));
    QVERIFY(!errorMessage.isEmpty());
    QVERIFY(Pet::instance().getProfiles().isEmpty());
}

void TestProfileDataMigrator::migrateLegacyMemory_whenSingleProfileAndValidLegacyDb_shouldCopyVerifyAndAtomicallyActivate() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ProfileMigrationRequest request = requestFor(directory);
    QVERIFY(QDir().mkpath(QFileInfo(request.legacyDatabasePath).path()));
    createLegacyDatabase(request.legacyDatabasePath);

    const ProfileMigrationResult result = ProfileDataMigrator().migrateLegacyMemory(request);

    QCOMPARE(result.status, ProfileMigrationStatus::Migrated);
    QVERIFY(result.profileStoreReady());
    QVERIFY(QFile::exists(result.activeDatabasePath));
    QVERIFY(QFile::exists(request.legacyDatabasePath));
    QVERIFY(!QFile::exists(result.activeDatabasePath + QStringLiteral(".tmp")));
}

void TestProfileDataMigrator::migrateLegacyMemory_whenMultipleProfilesAreAmbiguous_shouldPreserveLegacyDbAndRequireSelection() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ProfileMigrationRequest request = requestFor(directory);
    request.registeredProfileIds.append(
        QStringLiteral("f8685597-fc48-4df7-a15a-8ccfde643c52"));
    QVERIFY(QDir().mkpath(QFileInfo(request.legacyDatabasePath).path()));
    createLegacyDatabase(request.legacyDatabasePath);

    const ProfileMigrationResult result = ProfileDataMigrator().migrateLegacyMemory(request);

    QCOMPARE(result.status, ProfileMigrationStatus::Ambiguous);
    QVERIFY(!result.profileStoreReady());
    QCOMPARE(result.activeDatabasePath, request.legacyDatabasePath);
    QVERIFY(QFile::exists(request.legacyDatabasePath));
}

void TestProfileDataMigrator::migrateLegacyMemory_whenIntegrityCheckFails_shouldRemoveTempCopyAndKeepLegacyPath() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ProfileMigrationRequest request = requestFor(directory);
    QVERIFY(QDir().mkpath(QFileInfo(request.legacyDatabasePath).path()));
    QFile corrupt(request.legacyDatabasePath);
    QVERIFY(corrupt.open(QIODevice::WriteOnly));
    QCOMPARE(corrupt.write("not-a-sqlite-database"), qint64(21));
    corrupt.close();

    const ProfileMigrationResult result = ProfileDataMigrator().migrateLegacyMemory(request);
    const QString target = QDir(request.appDataRoot).filePath(
        QStringLiteral("profiles/%1/memory.db").arg(kProfileId));

    QCOMPARE(result.status, ProfileMigrationStatus::Failed);
    QVERIFY(!result.profileStoreReady());
    QVERIFY(QFile::exists(request.legacyDatabasePath));
    QVERIFY(!QFile::exists(target + QStringLiteral(".tmp")));
}

void TestProfileDataMigrator::migrateLegacyMemory_whenTargetAlreadyCommitted_shouldRemainIdempotent() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ProfileMigrationRequest request = requestFor(directory);
    const QString target = QDir(request.appDataRoot).filePath(
        QStringLiteral("profiles/%1/memory.db").arg(kProfileId));
    QVERIFY(QDir().mkpath(QFileInfo(target).path()));
    createLegacyDatabase(target, 1);
    QVERIFY(QDir().mkpath(QFileInfo(request.legacyDatabasePath).path()));
    createLegacyDatabase(request.legacyDatabasePath, 3);

    const ProfileMigrationResult result = ProfileDataMigrator().migrateLegacyMemory(request);

    QCOMPARE(result.status, ProfileMigrationStatus::AlreadyMigrated);
    QVERIFY(result.profileStoreReady());
    QCOMPARE(result.activeDatabasePath, target);
    QVERIFY(QFile::exists(request.legacyDatabasePath));
}

void TestProfileDataMigrator::migrateLegacyMemory_whenOnlyLegacyJsonExists_shouldImportIntoProfileDatabase() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ProfileMigrationRequest request = requestFor(directory);
    QVERIFY(QDir().mkpath(QFileInfo(request.legacyJsonPath).path()));
    const QJsonArray entries{
        QJsonObject{{QStringLiteral("id"), QStringLiteral("one")},
                    {QStringLiteral("type"), QStringLiteral("episodic")},
                    {QStringLiteral("summary"), QStringLiteral("first")}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("two")},
                    {QStringLiteral("type"), QStringLiteral("semantic")},
                    {QStringLiteral("summary"), QStringLiteral("second")}},
    };
    QFile jsonFile(request.legacyJsonPath);
    QVERIFY(jsonFile.open(QIODevice::WriteOnly));
    QVERIFY(jsonFile.write(QJsonDocument(entries).toJson()) > 0);
    jsonFile.close();

    const ProfileMigrationResult result = ProfileDataMigrator().migrateLegacyMemory(request);

    QCOMPARE(result.status, ProfileMigrationStatus::Migrated);
    QVERIFY(result.profileStoreReady());
    QVERIFY(QFile::exists(result.activeDatabasePath));
    QVERIFY(QFile::exists(request.legacyJsonPath));
}

void TestProfileDataMigrator::migrateLegacyMemory_whenRegisteredProfilesAreInvalid_shouldRejectRequest() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ProfileMigrationRequest request = requestFor(directory);
    request.registeredProfileIds = {kProfileId, kProfileId};
    QVERIFY(QDir().mkpath(QFileInfo(request.legacyDatabasePath).path()));
    createLegacyDatabase(request.legacyDatabasePath);

    const ProfileMigrationResult result = ProfileDataMigrator().migrateLegacyMemory(request);

    QCOMPARE(result.status, ProfileMigrationStatus::Failed);
    QVERIFY(!result.profileStoreReady());
    QVERIFY(QFile::exists(request.legacyDatabasePath));
}

QTEST_GUILESS_MAIN(TestProfileDataMigrator)

#include "test_profile_data_migrator.moc"
