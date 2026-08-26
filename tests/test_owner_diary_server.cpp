#include <QtTest>

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QLocalSocket>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtEndian>

#ifdef Q_OS_WIN
#include <windows.h>
#include <aclapi.h>
#endif

#include "ai/context_builder.h"
#include "ai/owner/owner_diary_facade.h"
#include "ai/owner/owner_diary_protocol.h"
#include "ai/owner/owner_diary_server.h"
#include "ai/reflection/diary_service.h"
#include "ai/reflection/private_key_provider.h"
#include "ai/reflection/private_psyche_crypto.h"
#include "ai/reflection/sqlite_private_psyche_repository.h"
#include "ai/tool_registry.h"

namespace {

const QString kProfileId = QStringLiteral("11111111-1111-4111-8111-111111111111");
const QString kOtherProfileId =
    QStringLiteral("22222222-2222-4222-8222-222222222222");

QByteArray base64Url(const QByteArray& bytes) {
    return bytes.toBase64(
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

class TestKeyProvider final : public PrivateKeyProvider {
public:
    Result<PrivateKeyMaterial, DomainError> loadOrCreate(
        const QString& profileId) override {
        return Result<PrivateKeyMaterial, DomainError>::success(
            PrivateKeyMaterial{profileId, 1, QByteArray(32, 'k')});
    }
};

class TestCrypto final : public PrivatePsycheCrypto {
public:
    Result<EncryptedPrivatePayload, DomainError> encrypt(
        const QByteArray& plaintext,
        const PrivateRecordAad& aad,
        const PrivateKeyMaterial& key) const override {
        EncryptedPrivatePayload encrypted;
        encrypted.schemaVersion = aad.schemaVersion;
        encrypted.keyVersion = key.keyVersion;
        encrypted.nonce = QByteArrayLiteral("owner-test-nonce");
        const QByteArray tag = QCryptographicHash::hash(
            key.key + aad.toBytes() + encrypted.nonce + plaintext,
            QCryptographicHash::Sha256);
        encrypted.ciphertext = plaintext + tag;
        return Result<EncryptedPrivatePayload, DomainError>::success(encrypted);
    }

    Result<QByteArray, DomainError> decrypt(
        const EncryptedPrivatePayload& encrypted,
        const PrivateRecordAad& aad,
        const PrivateKeyMaterial& key) const override {
        if (encrypted.ciphertext.size() < 32) {
            return Result<QByteArray, DomainError>::failure(
                domainError(QStringLiteral("PRIVATE_AUTH_FAILED"),
                            QStringLiteral("ciphertext is truncated")));
        }
        const QByteArray plaintext = encrypted.ciphertext.chopped(32);
        const QByteArray expected = QCryptographicHash::hash(
            key.key + aad.toBytes() + encrypted.nonce + plaintext,
            QCryptographicHash::Sha256);
        if (!ownerDiaryConstantTimeEquals(
                expected, encrypted.ciphertext.right(32))) {
            return Result<QByteArray, DomainError>::failure(
                domainError(QStringLiteral("PRIVATE_AUTH_FAILED"),
                            QStringLiteral("ciphertext authentication failed")));
        }
        return Result<QByteArray, DomainError>::success(plaintext);
    }
};

struct OwnerFixture {
    QTemporaryDir directory;
    SqlitePrivatePsycheRepository repository;
    TestKeyProvider keys;
    TestCrypto crypto;
    std::unique_ptr<DiaryService> diary;
    std::unique_ptr<OwnerDiaryFacade> facade;

    bool open() {
        if (!directory.isValid()
            || !repository.open(
                    directory.filePath(QStringLiteral("private_psyche.sqlite"))).isOk()) {
            return false;
        }
        diary = std::make_unique<DiaryService>(
            kProfileId, nullptr, nullptr, &keys, &crypto, &repository);
        facade = std::make_unique<OwnerDiaryFacade>(diary.get());
        return true;
    }

    QString insertDiary(
        const QString& profileId,
        const QDate& localDate,
        const QString& body,
        const QJsonObject& index = {{QStringLiteral("theme"),
                                     QStringLiteral("companionship")}}) {
        const QString entryId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QDateTime createdAt = QDateTime(
            localDate, QTime(23, 30), Qt::UTC);
        const QJsonObject envelope{
            {QStringLiteral("body"), body},
            {QStringLiteral("index"), index},
            {QStringLiteral("localDate"), localDate.toString(Qt::ISODate)},
            {QStringLiteral("sourceCutoffSequence"), 7},
            {QStringLiteral("createdAt"), createdAt.toString(Qt::ISODateWithMs)}
        };
        const PrivateKeyMaterial key{profileId, 1, QByteArray(32, 'k')};
        const PrivateRecordAad aad{
            1, profileId, QStringLiteral("diary_entry"), entryId, 1};
        const auto encrypted = crypto.encrypt(
            QJsonDocument(envelope).toJson(QJsonDocument::Compact), aad, key);
        if (!encrypted.isOk()) return {};

        QSqlQuery query(QSqlDatabase::database(repository.connectionName()));
        query.prepare(QStringLiteral(
            "INSERT INTO diary_entry(entry_id,profile_id,local_date,key_version,"
            "nonce,ciphertext,index_json,ciphertext_hash,source_cutoff_sequence,created_at) "
            "VALUES(?,?,?,?,?,?,?,?,?,?)"));
        query.addBindValue(entryId);
        query.addBindValue(profileId);
        query.addBindValue(localDate.toString(Qt::ISODate));
        query.addBindValue(1);
        query.addBindValue(encrypted.value().nonce);
        query.addBindValue(encrypted.value().ciphertext);
        query.addBindValue(QString::fromUtf8(
            QJsonDocument(index).toJson(QJsonDocument::Compact)));
        query.addBindValue(QString::fromLatin1(QCryptographicHash::hash(
            encrypted.value().ciphertext, QCryptographicHash::Sha256).toHex()));
        query.addBindValue(7);
        query.addBindValue(createdAt.toString(Qt::ISODateWithMs));
        return query.exec() ? entryId : QString{};
    }
};

QString socketName() {
    return QStringLiteral("desktop-pet-owner-test-%1-%2")
        .arg(kProfileId,
             QUuid::createUuid().toString(QUuid::WithoutBraces));
}

QByteArray capabilityToken() {
    return QCryptographicHash::hash(
        QUuid::createUuid().toByteArray() + QUuid::createUuid().toByteArray(),
        QCryptographicHash::Sha256);
}

#ifdef Q_OS_WIN
bool protectForCurrentWindowsUser(const QString& path) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    QByteArray buffer(static_cast<int>(size), Qt::Uninitialized);
    const bool tokenRead = GetTokenInformation(
        token, TokenUser, buffer.data(), size, &size);
    CloseHandle(token);
    if (!tokenRead) return false;

    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = GENERIC_ALL;
    access.grfAccessMode = SET_ACCESS;
    access.grfInheritance = NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(
        reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid);
    PACL acl = nullptr;
    if (SetEntriesInAclW(1, &access, nullptr, &acl) != ERROR_SUCCESS) {
        return false;
    }
    const std::wstring native = path.toStdWString();
    const DWORD result = SetNamedSecurityInfoW(
        const_cast<wchar_t*>(native.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, acl, nullptr);
    LocalFree(acl);
    return result == ERROR_SUCCESS;
}
#endif

QString writeBootstrap(
    OwnerFixture& fixture,
    const QString& socket,
    const QByteArray& token,
    const QString& profileId = kProfileId,
    const QDateTime& expiresAt = QDateTime::currentDateTimeUtc().addSecs(60),
    QFileDevice::Permissions permissions =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner) {
    const QString path = fixture.directory.filePath(
        QStringLiteral("owner-bootstrap-%1.json")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return {};
    const QJsonObject object{
        {QStringLiteral("protocolVersion"), 1},
        {QStringLiteral("profileId"), profileId},
        {QStringLiteral("socketName"), socket},
        {QStringLiteral("capabilityToken"), QString::fromLatin1(base64Url(token))},
        {QStringLiteral("expiresAt"), expiresAt.toString(Qt::ISODateWithMs)},
        {QStringLiteral("maxFrameBytes"), 65536},
        {QStringLiteral("sessionTtlSeconds"), 300}
    };
    file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    file.flush();
    file.close();
    QFile::setPermissions(path, permissions);
#ifdef Q_OS_WIN
    if (!protectForCurrentWindowsUser(path)) {
        QFile::remove(path);
        return {};
    }
#endif
    return path;
}

bool connectSocket(QLocalSocket& socket, const QString& name) {
    socket.connectToServer(name);
    if (!socket.waitForConnected(1000)) return false;
    QCoreApplication::processEvents();
    return true;
}

QJsonObject exchange(QLocalSocket& socket, const QJsonObject& request) {
    const QByteArray frame = encodeOwnerDiaryFrame(request);
    if (socket.write(frame) != frame.size() || !socket.waitForBytesWritten(1000)) {
        return {};
    }
    QElapsedTimer timer;
    timer.start();
    while (socket.bytesAvailable() < 4 && timer.elapsed() < 1000) {
        QCoreApplication::processEvents();
        socket.waitForReadyRead(10);
    }
    if (socket.bytesAvailable() < 4) return {};
    const QByteArray prefix = socket.read(4);
    const quint32 size = qFromBigEndian<quint32>(
        reinterpret_cast<const uchar*>(prefix.constData()));
    QByteArray payload;
    while (payload.size() < static_cast<int>(size)) {
        if (socket.bytesAvailable() == 0) {
            QCoreApplication::processEvents();
            if (!socket.waitForReadyRead(1000)) return {};
        }
        payload += socket.read(size - payload.size());
    }
    const QJsonDocument document = QJsonDocument::fromJson(payload);
    return document.isObject() ? document.object() : QJsonObject{};
}

QJsonObject request(
    const QString& requestId,
    const QString& action,
    const QJsonObject& payload) {
    return {
        {QStringLiteral("protocolVersion"), 1},
        {QStringLiteral("requestId"), requestId},
        {QStringLiteral("action"), action},
        {QStringLiteral("payload"), payload}
    };
}

QString authenticate(
    QLocalSocket& socket,
    const QString& socketNameValue,
    const QByteArray& token) {
    if (!connectSocket(socket, socketNameValue)) return {};
    const QJsonObject response = exchange(
        socket,
        request(QStringLiteral("hello-1"), QStringLiteral("hello"), {
            {QStringLiteral("capabilityToken"),
             QString::fromLatin1(base64Url(token))},
            {QStringLiteral("clientNonce"),
             QString::fromLatin1(base64Url(QByteArray(16, 'n')))}
        }));
    if (!response.value(QStringLiteral("ok")).toBool()) return {};
    return response.value(QStringLiteral("data"))
        .toObject().value(QStringLiteral("sessionToken")).toString();
}

} // namespace

class OwnerDiaryServerTests : public QObject {
    Q_OBJECT

private slots:
    void list_whenOwnerSessionValid_shouldReturnPagedMetadataWithoutBody();
    void list_whenProfileDoesNotMatch_shouldReturnGenericNotFound();
    void get_whenOwnerSessionValid_shouldReturnOneDecryptedEntry();
    void get_whenEntryBelongsToAnotherProfile_shouldNotRevealExistence();
    void listen_whenBootstrapIsValid_shouldConsumeSecretAndAcceptAuthenticatedHello();
    void listen_whenBootstrapOwnerOrExpiryInvalid_shouldRejectAndNotBindSocket();
    void listen_whenTokenIsForgedOrReplayed_shouldRejectSession();
    void handleFrame_whenListOrGetActionValid_shouldReturnMatchingReadOnlyResponse();
    void handleFrame_whenActionIsWriteSqlPathSkillOrTool_shouldReturnActionNotAllowed();
    void handleFrame_whenFrameTooLargeOrVersionInvalid_shouldRejectWithoutAllocationGrowth();
    void handleFrame_whenRequestIdIsRetried_shouldReturnIdempotentReadResponse();
    void stop_whenSessionsExist_shouldCloseSocketAndRevokeSessionTokens();
    void toolRegistry_whenOwnerDiaryEnabled_shouldNotExposeOwnerDiaryActions();
    void dialogueContext_whenOwnerReadsDiary_shouldNotContainAccessEventOrDiaryBody();
};

void OwnerDiaryServerTests::list_whenOwnerSessionValid_shouldReturnPagedMetadataWithoutBody() {
    OwnerFixture fixture;
    QVERIFY(fixture.open());
    QVERIFY(!fixture.insertDiary(kProfileId, QDate(2026, 8, 24),
                                 QStringLiteral("private body one")).isEmpty());
    QVERIFY(!fixture.insertDiary(kProfileId, QDate(2026, 8, 25),
                                 QStringLiteral("private body two")).isEmpty());
    DiaryListQuery query;
    query.limit = 1;
    const auto first = fixture.facade->list(
        query, OwnerAuthContext{kProfileId, true});
    QVERIFY(first.isOk());
    QCOMPARE(first.value().entries.size(), 1);
    QCOMPARE(first.value().entries.first().localDate, QDate(2026, 8, 25));
    QVERIFY(!first.value().nextCursor.isEmpty());
    QVERIFY(!QJsonDocument(first.value().entries.first().index)
                 .toJson(QJsonDocument::Compact).contains("private body"));

    query.cursor = first.value().nextCursor;
    const auto second = fixture.facade->list(
        query, OwnerAuthContext{kProfileId, true});
    QVERIFY(second.isOk());
    QCOMPARE(second.value().entries.size(), 1);
    QCOMPARE(second.value().entries.first().localDate, QDate(2026, 8, 24));
    QVERIFY(second.value().nextCursor.isEmpty());

    query.cursor = QStringLiteral("not-a-valid-cursor");
    const auto malformed = fixture.facade->list(
        query, OwnerAuthContext{kProfileId, true});
    QVERIFY(!malformed.isOk());
    QCOMPARE(malformed.error().code, QStringLiteral("DIARY_QUERY_INVALID"));
}

void OwnerDiaryServerTests::list_whenProfileDoesNotMatch_shouldReturnGenericNotFound() {
    OwnerFixture fixture;
    QVERIFY(fixture.open());
    const auto result = fixture.facade->list(
        DiaryListQuery{}, OwnerAuthContext{kOtherProfileId, true});
    QVERIFY(!result.isOk());
    QCOMPARE(result.error().code, QStringLiteral("DIARY_ENTRY_NOT_FOUND"));
}

void OwnerDiaryServerTests::get_whenOwnerSessionValid_shouldReturnOneDecryptedEntry() {
    OwnerFixture fixture;
    QVERIFY(fixture.open());
    const QString id = fixture.insertDiary(
        kProfileId, QDate(2026, 8, 25), QStringLiteral("only this body"));
    const auto result = fixture.facade->get(
        id, OwnerAuthContext{kProfileId, true});
    QVERIFY(result.isOk());
    QCOMPARE(result.value().entryId, id);
    QCOMPARE(result.value().body, QStringLiteral("only this body"));
}

void OwnerDiaryServerTests::get_whenEntryBelongsToAnotherProfile_shouldNotRevealExistence() {
    OwnerFixture fixture;
    QVERIFY(fixture.open());
    const QString id = fixture.insertDiary(
        kOtherProfileId, QDate(2026, 8, 25), QStringLiteral("other body"));
    const auto result = fixture.facade->get(
        id, OwnerAuthContext{kProfileId, true});
    QVERIFY(!result.isOk());
    QCOMPARE(result.error().code, QStringLiteral("DIARY_ENTRY_NOT_FOUND"));
}

void OwnerDiaryServerTests::listen_whenBootstrapIsValid_shouldConsumeSecretAndAcceptAuthenticatedHello() {
    OwnerFixture fixture;
    QVERIFY(fixture.open());
    const QString name = socketName();
    const QByteArray token = capabilityToken();
    const QString path = writeBootstrap(fixture, name, token);
    const auto bootstrap = consumeOwnerDiaryBootstrap(path, kProfileId);
    QVERIFY(bootstrap.isOk());
    QVERIFY(!QFile::exists(path));
    QCOMPARE(bootstrap.value().capabilityToken, token);

    OwnerDiaryServer server(fixture.facade.get(), kProfileId);
    QVERIFY(server.listen(name, token, 65536, 300).isOk());
    QLocalSocket socket;
    const QString session = authenticate(socket, name, token);
    QVERIFY(!session.isEmpty());
    QCOMPARE(server.sessionCount(), 1);
}

void OwnerDiaryServerTests::listen_whenBootstrapOwnerOrExpiryInvalid_shouldRejectAndNotBindSocket() {
    OwnerFixture fixture;
    QVERIFY(fixture.open());
    const QString name = socketName();
    const QByteArray token = capabilityToken();
    const QString expired = writeBootstrap(
        fixture, name, token, kProfileId,
        QDateTime::currentDateTimeUtc().addSecs(-1));
    const auto result = consumeOwnerDiaryBootstrap(expired, kProfileId);
    QVERIFY(!result.isOk());
    QVERIFY(!QFile::exists(expired));

    const QString wrongProfile = writeBootstrap(
        fixture, name, token, kOtherProfileId);
    const auto wrongProfileResult = consumeOwnerDiaryBootstrap(
        wrongProfile, kProfileId);
    QVERIFY(!wrongProfileResult.isOk());
    QVERIFY(!QFile::exists(wrongProfile));

#ifdef Q_OS_UNIX
    const QString insecure = writeBootstrap(
        fixture, name, token, kProfileId,
        QDateTime::currentDateTimeUtc().addSecs(60),
        QFileDevice::ReadOwner | QFileDevice::WriteOwner
            | QFileDevice::ReadGroup);
    const auto insecureResult = consumeOwnerDiaryBootstrap(insecure, kProfileId);
    QVERIFY(!insecureResult.isOk());
    QFile::remove(insecure);
#endif
    OwnerDiaryServer server(fixture.facade.get(), kProfileId);
    QVERIFY(!server.isListening());
}

void OwnerDiaryServerTests::listen_whenTokenIsForgedOrReplayed_shouldRejectSession() {
    OwnerFixture fixture;
    QVERIFY(fixture.open());
    const QString name = socketName();
    const QByteArray token = capabilityToken();
    OwnerDiaryServer server(fixture.facade.get(), kProfileId);
    QVERIFY(server.listen(name, token, 65536, 300).isOk());

    QLocalSocket forged;
    QVERIFY(connectSocket(forged, name));
    const auto forgedResponse = exchange(
        forged, request(QStringLiteral("forged"), QStringLiteral("hello"), {
            {QStringLiteral("capabilityToken"),
             QString::fromLatin1(base64Url(capabilityToken()))},
            {QStringLiteral("clientNonce"), QStringLiteral("nonce")}
        }));
    QVERIFY(!forgedResponse.value(QStringLiteral("ok")).toBool());

    QLocalSocket authenticated;
    QVERIFY(!authenticate(authenticated, name, token).isEmpty());
    QLocalSocket replay;
    QVERIFY(connectSocket(replay, name));
    const auto replayResponse = exchange(
        replay, request(QStringLiteral("replay"), QStringLiteral("hello"), {
            {QStringLiteral("capabilityToken"),
             QString::fromLatin1(base64Url(token))},
            {QStringLiteral("clientNonce"), QStringLiteral("nonce")}
        }));
    QVERIFY(!replayResponse.value(QStringLiteral("ok")).toBool());
}

void OwnerDiaryServerTests::handleFrame_whenListOrGetActionValid_shouldReturnMatchingReadOnlyResponse() {
    OwnerFixture fixture;
    QVERIFY(fixture.open());
    const QString entryId = fixture.insertDiary(
        kProfileId, QDate(2026, 8, 25), QStringLiteral("selected diary"));
    const QString name = socketName();
    const QByteArray token = capabilityToken();
    OwnerDiaryServer server(fixture.facade.get(), kProfileId);
    QVERIFY(server.listen(name, token, 65536, 300).isOk());
    QLocalSocket socket;
    const QString session = authenticate(socket, name, token);
    QVERIFY(!session.isEmpty());

    const auto listed = exchange(socket, request(
        QStringLiteral("list-1"), QStringLiteral("list_diary_entries"), {
            {QStringLiteral("sessionToken"), session},
            {QStringLiteral("from"), QString()},
            {QStringLiteral("to"), QString()},
            {QStringLiteral("cursor"), QString()},
            {QStringLiteral("limit"), 20}
        }));
    QVERIFY(listed.value(QStringLiteral("ok")).toBool());
    const QJsonArray entries = listed.value(QStringLiteral("data"))
                                   .toObject().value(QStringLiteral("entries")).toArray();
    QCOMPARE(entries.size(), 1);
    QVERIFY(!entries.first().toObject().contains(QStringLiteral("body")));

    const auto selected = exchange(socket, request(
        QStringLiteral("get-1"), QStringLiteral("get_diary_entry"), {
            {QStringLiteral("sessionToken"), session},
            {QStringLiteral("entryId"), entryId}
        }));
    QVERIFY(selected.value(QStringLiteral("ok")).toBool());
    QCOMPARE(selected.value(QStringLiteral("data"))
                 .toObject().value(QStringLiteral("body")).toString(),
             QStringLiteral("selected diary"));
}

void OwnerDiaryServerTests::handleFrame_whenActionIsWriteSqlPathSkillOrTool_shouldReturnActionNotAllowed() {
    OwnerFixture fixture;
    QVERIFY(fixture.open());
    const QString name = socketName();
    const QByteArray token = capabilityToken();
    OwnerDiaryServer server(fixture.facade.get(), kProfileId);
    QVERIFY(server.listen(name, token, 65536, 300).isOk());
    QLocalSocket socket;
    const QString session = authenticate(socket, name, token);
    for (const QString& action : {
             QStringLiteral("create_diary"), QStringLiteral("sql"),
             QStringLiteral("read_path"), QStringLiteral("approve_skill"),
             QStringLiteral("run_tool")}) {
        const auto response = exchange(socket, request(
            QUuid::createUuid().toString(QUuid::WithoutBraces), action,
            {{QStringLiteral("sessionToken"), session},
             {QStringLiteral("path"), QStringLiteral("/tmp/private")}}));
        QVERIFY(!response.value(QStringLiteral("ok")).toBool());
        QCOMPARE(response.value(QStringLiteral("error"))
                     .toObject().value(QStringLiteral("code")).toString(),
                 QStringLiteral("ACTION_NOT_ALLOWED"));
    }
}

void OwnerDiaryServerTests::handleFrame_whenFrameTooLargeOrVersionInvalid_shouldRejectWithoutAllocationGrowth() {
    OwnerFixture fixture;
    QVERIFY(fixture.open());
    const QString largeEntry = fixture.insertDiary(
        kProfileId, QDate(2026, 8, 25), QString(4096, QLatin1Char('x')));
    QVERIFY(!largeEntry.isEmpty());
    const QString name = socketName();
    const QByteArray token = capabilityToken();
    OwnerDiaryServer server(fixture.facade.get(), kProfileId);
    QVERIFY(server.listen(name, token, 1024, 300).isOk());

    QLocalSocket oversized;
    QVERIFY(connectSocket(oversized, name));
    QByteArray prefix(4, Qt::Uninitialized);
    qToBigEndian<quint32>(1025,
        reinterpret_cast<uchar*>(prefix.data()));
    oversized.write(prefix);
    QVERIFY(oversized.waitForBytesWritten(1000));
    QTRY_VERIFY_WITH_TIMEOUT(
        oversized.state() == QLocalSocket::UnconnectedState, 1000);
    QVERIFY(server.bufferedBytesForTesting() <= 4);

    QLocalSocket flooded;
    QVERIFY(connectSocket(flooded, name));
    flooded.write(QByteArray(4096, 'x'));
    flooded.flush();
    QTRY_VERIFY_WITH_TIMEOUT(
        flooded.state() == QLocalSocket::UnconnectedState, 1000);
    QVERIFY(server.bufferedBytesForTesting() <= 4);

    QLocalSocket invalidVersion;
    QVERIFY(connectSocket(invalidVersion, name));
    QJsonObject invalid = request(
        QStringLiteral("version-2"), QStringLiteral("hello"), {});
    invalid.insert(QStringLiteral("protocolVersion"), 2);
    const auto response = exchange(invalidVersion, invalid);
    QVERIFY(!response.value(QStringLiteral("ok")).toBool());
    QCOMPARE(response.value(QStringLiteral("error"))
                 .toObject().value(QStringLiteral("code")).toString(),
             QStringLiteral("PROTOCOL_VERSION_UNSUPPORTED"));

    QLocalSocket authenticated;
    const QString session = authenticate(authenticated, name, token);
    QVERIFY(!session.isEmpty());
    const auto largeResponse = exchange(authenticated, request(
        QStringLiteral("large-response"), QStringLiteral("get_diary_entry"), {
            {QStringLiteral("sessionToken"), session},
            {QStringLiteral("entryId"), largeEntry}
        }));
    QVERIFY(!largeResponse.value(QStringLiteral("ok")).toBool());
    QCOMPARE(largeResponse.value(QStringLiteral("error"))
                 .toObject().value(QStringLiteral("code")).toString(),
             QStringLiteral("DIARY_RESPONSE_TOO_LARGE"));
    QCOMPARE(server.cachedResponseBytesForTesting(), qsizetype(0));

    for (int index = 0; index < 10; ++index) {
        const auto listed = exchange(authenticated, request(
            QStringLiteral("bounded-cache-%1").arg(index),
            QStringLiteral("list_diary_entries"), {
                {QStringLiteral("sessionToken"), session},
                {QStringLiteral("limit"), 1}
            }));
        QVERIFY(listed.value(QStringLiteral("ok")).toBool());
        QVERIFY(server.cachedResponseBytesForTesting() <= 2048);
    }
}

void OwnerDiaryServerTests::handleFrame_whenRequestIdIsRetried_shouldReturnIdempotentReadResponse() {
    OwnerFixture fixture;
    QVERIFY(fixture.open());
    fixture.insertDiary(kProfileId, QDate(2026, 8, 25), QStringLiteral("body"));
    const QString name = socketName();
    const QByteArray token = capabilityToken();
    OwnerDiaryServer server(fixture.facade.get(), kProfileId);
    QVERIFY(server.listen(name, token, 65536, 300).isOk());
    QLocalSocket socket;
    const QString session = authenticate(socket, name, token);
    const QJsonObject listRequest = request(
        QStringLiteral("same-request"), QStringLiteral("list_diary_entries"), {
            {QStringLiteral("sessionToken"), session},
            {QStringLiteral("limit"), 10}
        });
    const auto first = exchange(socket, listRequest);
    const auto second = exchange(socket, listRequest);
    QCOMPARE(QJsonDocument(first).toJson(QJsonDocument::Compact),
             QJsonDocument(second).toJson(QJsonDocument::Compact));

    for (int index = 0; index < 117; ++index) {
        const auto replayed = exchange(socket, listRequest);
        QVERIFY(replayed.value(QStringLiteral("ok")).toBool());
    }
    const auto rateLimited = exchange(socket, listRequest);
    QVERIFY(!rateLimited.value(QStringLiteral("ok")).toBool());
    QCOMPARE(rateLimited.value(QStringLiteral("error"))
                 .toObject().value(QStringLiteral("code")).toString(),
             QStringLiteral("OWNER_RATE_LIMITED"));
}

void OwnerDiaryServerTests::stop_whenSessionsExist_shouldCloseSocketAndRevokeSessionTokens() {
    OwnerFixture fixture;
    QVERIFY(fixture.open());
    const QString name = socketName();
    const QByteArray token = capabilityToken();
    OwnerDiaryServer server(fixture.facade.get(), kProfileId);
    QVERIFY(server.listen(name, token, 65536, 300).isOk());
    QLocalSocket socket;
    QVERIFY(!authenticate(socket, name, token).isEmpty());
    server.stop();
    QVERIFY(!server.isListening());
    QCOMPARE(server.sessionCount(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(
        socket.state() == QLocalSocket::UnconnectedState, 1000);
}

void OwnerDiaryServerTests::toolRegistry_whenOwnerDiaryEnabled_shouldNotExposeOwnerDiaryActions() {
    ToolRegistry registry;
    const QByteArray schemas = QJsonDocument(registry.allToolSchemas())
                                   .toJson(QJsonDocument::Compact);
    QVERIFY(!schemas.contains("diary"));
    QVERIFY(!registry.hasTool(QStringLiteral("list_diary_entries")));
    QVERIFY(!registry.hasTool(QStringLiteral("get_diary_entry")));
}

void OwnerDiaryServerTests::dialogueContext_whenOwnerReadsDiary_shouldNotContainAccessEventOrDiaryBody() {
    OwnerFixture fixture;
    QVERIFY(fixture.open());
    const QString body = QStringLiteral("never enter dialogue context");
    const QString id = fixture.insertDiary(kProfileId, QDate(2026, 8, 25), body);
    QVERIFY(fixture.facade->get(id, OwnerAuthContext{kProfileId, true}).isOk());
    ContextBuilder builder;
    const QString context = builder.buildRuntimeContext(
        QStringLiteral("pet"), QStringLiteral("manual"));
    QVERIFY(!context.contains(body));
    QVERIFY(!context.contains(QStringLiteral("OwnerDiary")));
    QVERIFY(!context.contains(id));
}

QTEST_GUILESS_MAIN(OwnerDiaryServerTests)
#include "test_owner_diary_server.moc"
