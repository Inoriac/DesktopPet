#include <QtTest>

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <atomic>

#include <memory>
#include <optional>

#include "ai/chat/profile_chat_history_store.h"
#include "ai/chat/conversation_store.h"
#include <QJsonArray>
#include "ai/event/runtime_unit_of_work.h"

namespace {

const QString kProfileId =
    QStringLiteral("5bb00e6d-937a-4f46-9c87-e3933c078f5a");
const QString kOtherProfileId =
    QStringLiteral("f8685597-fc48-4df7-a15a-8ccfde643c52");

ProfileChatStoreOptions optionsFor(const QTemporaryDir& directory,
                                   const QString& profileId = kProfileId) {
    ProfileChatStoreOptions options;
    options.appDataRoot = directory.filePath(QStringLiteral("app-data"));
    options.profileId = profileId;
    options.registeredProfileIds = {profileId};
    options.legacyHistoryPath = directory.filePath(QStringLiteral("log/chat_history.jsonl"));
    return options;
}

QString historyPath(const ProfileChatStoreOptions& options) {
    return QDir(options.appDataRoot).filePath(
        QStringLiteral("profiles/%1/chat_history.jsonl").arg(options.profileId));
}

ChatHistoryEntry entry(const QString& id,
                       const QString& role,
                       const QString& content,
                       ChatMessageStatus status = ChatMessageStatus::Complete) {
    ChatHistoryEntry value;
    value.id = id;
    value.role = role;
    value.content = content;
    value.timestamp = QDateTime::fromString(
        QStringLiteral("2026-08-27T12:00:00.000+08:00"), Qt::ISODateWithMs);
    value.status = status;
    return value;
}

QByteArray encodedLine(const ChatHistoryEntry& value, bool includeReplyToId = true) {
    QJsonObject object{
        {QStringLiteral("schemaVersion"), 2},
        {QStringLiteral("id"), value.id},
        {QStringLiteral("role"), value.role},
        {QStringLiteral("content"), value.content},
        {QStringLiteral("timestamp"), value.timestamp.toString(Qt::ISODateWithMs)},
        {QStringLiteral("status"), QStringLiteral("complete")},
    };
    if (includeReplyToId && !value.replyToId.isEmpty()) {
        object.insert(QStringLiteral("replyToId"), value.replyToId);
    }
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

int nonEmptyLineCount(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return 0;
    int count = 0;
    while (!file.atEnd()) {
        if (!file.readLine().trimmed().isEmpty()) ++count;
    }
    return count;
}

} // namespace

class TestChatHistory : public QObject {
    Q_OBJECT

private slots:
    void queryHistory_data();
    void queryHistory();
    void queryHistory_sqliteFullArchiveAndScope();
    void initTestCase();
    void cleanup();
    void openAndLoad_whenProfilesDiffer_shouldKeepHistoriesIsolated();
    void openAndLoad_whenSingleProfileOwnsLegacyHistory_shouldImportOnceAndKeepLegacyFile();
    void openAndLoad_whenMultipleProfilesExist_shouldNotImportLegacyHistory();
    void open_whenHistoryExists_shouldRestrictPermissionsToOwner();
    void load_whenLineIsCorruptOversizedOrPartial_shouldSkipItAndContinue();
    void load_whenReplyToIdIsMissingInLegacyEntry_shouldUseEmptyValue();
    void load_whenStatusIsInvalidOrIdDuplicated_shouldKeepFirstValidTerminalEntry();
    void appendFinal_whenEntryIsValid_shouldRoundTripSchemaVersionTwoFields();
    void appendFinal_whenAssistantHasReplyToId_shouldRoundTripReplyLink();
    void appendFinal_whenHistoryEndsWithPartialLine_shouldWriteIndependentValidLine();
    void appendFinal_whenEntryIsNonTerminal_shouldRejectWithoutWriting();
    void v2History_importsOnceAndWritesOnlySqliteAcrossThreadCopies();
    void historyMigration_v2Failure_neverFallsBackToJsonlWrites();
    void historyMigration_completedSqlite_doesNotRequireLegacyFile();
    void historyMigration_sourceReadFailure_doesNotCompleteEmptyImport();
    void appendFinal_whenNonAssistantHasReplyToId_shouldRejectWithoutWriting();

private:
    std::unique_ptr<QTemporaryDir> m_settingsDirectory;
};

void TestChatHistory::initTestCase() {
    m_settingsDirectory = std::make_unique<QTemporaryDir>();
    QVERIFY(m_settingsDirectory->isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       m_settingsDirectory->path());
    QCoreApplication::setOrganizationName(QStringLiteral("Desktop Pet Team Test"));
    QCoreApplication::setApplicationName(QStringLiteral("Chat History Tests"));
}

void TestChatHistory::cleanup() {
    QSettings settings;
    settings.clear();
    settings.sync();
}

void TestChatHistory::openAndLoad_whenProfilesDiffer_shouldKeepHistoriesIsolated() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ProfileChatStoreOptions firstOptions = optionsFor(directory);
    firstOptions.registeredProfileIds = {kProfileId, kOtherProfileId};
    ProfileChatStoreOptions secondOptions = optionsFor(directory, kOtherProfileId);
    secondOptions.registeredProfileIds = firstOptions.registeredProfileIds;

    ProfileChatHistoryStore first;
    ProfileChatHistoryStore second;
    QString error;
    QVERIFY2(first.open(firstOptions, &error), qPrintable(error));
    QVERIFY2(second.open(secondOptions, &error), qPrintable(error));
    QVERIFY2(first.appendFinal(entry(QStringLiteral("first"), QStringLiteral("user"),
                                     QStringLiteral("one")), &error), qPrintable(error));
    QVERIFY2(second.appendFinal(entry(QStringLiteral("second"), QStringLiteral("user"),
                                      QStringLiteral("two")), &error), qPrintable(error));

    const QList<ChatHistoryEntry> firstMessages = first.load(&error);
    const QList<ChatHistoryEntry> secondMessages = second.load(&error);
    QCOMPARE(firstMessages.size(), 1);
    QCOMPARE(firstMessages.first().id, QStringLiteral("first"));
    QCOMPARE(secondMessages.size(), 1);
    QCOMPARE(secondMessages.first().id, QStringLiteral("second"));
}

void TestChatHistory::openAndLoad_whenSingleProfileOwnsLegacyHistory_shouldImportOnceAndKeepLegacyFile() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const ProfileChatStoreOptions options = optionsFor(directory);
    QVERIFY(QDir().mkpath(QFileInfo(options.legacyHistoryPath).path()));
    QFile legacy(options.legacyHistoryPath);
    QVERIFY(legacy.open(QIODevice::WriteOnly | QIODevice::Text));
    legacy.write("{\"role\":\"assistant\",\"content\":\"legacy\","
                 "\"timestamp\":\"2026-08-27T12:00:00+08:00\"}\n");
    legacy.close();

    QString error;
    ProfileChatHistoryStore first;
    QVERIFY2(first.open(options, &error), qPrintable(error));
    const QList<ChatHistoryEntry> imported = first.load(&error);
    QCOMPARE(imported.size(), 1);
    QCOMPARE(imported.first().content, QStringLiteral("legacy"));
    QVERIFY(!imported.first().id.isEmpty());
    QVERIFY(QFile::exists(options.legacyHistoryPath));

    ProfileChatHistoryStore reopened;
    QVERIFY2(reopened.open(options, &error), qPrintable(error));
    QCOMPARE(reopened.load(&error).size(), 1);
    QCOMPARE(nonEmptyLineCount(historyPath(options)), 1);
    QVERIFY(QSettings().value(
        QStringLiteral("chat/%1/legacyImportedV1").arg(kProfileId)).toBool());
}

void TestChatHistory::openAndLoad_whenMultipleProfilesExist_shouldNotImportLegacyHistory() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ProfileChatStoreOptions options = optionsFor(directory);
    options.registeredProfileIds = {kProfileId, kOtherProfileId};
    QVERIFY(QDir().mkpath(QFileInfo(options.legacyHistoryPath).path()));
    QFile legacy(options.legacyHistoryPath);
    QVERIFY(legacy.open(QIODevice::WriteOnly | QIODevice::Text));
    legacy.write("{\"role\":\"assistant\",\"content\":\"ambiguous\","
                 "\"timestamp\":\"2026-08-27T12:00:00+08:00\"}\n");
    legacy.close();

    ProfileChatHistoryStore store;
    QString error;
    QVERIFY2(store.open(options, &error), qPrintable(error));
    QVERIFY(store.load(&error).isEmpty());
    QVERIFY(QFile::exists(options.legacyHistoryPath));
    QVERIFY(!QSettings().value(
        QStringLiteral("chat/%1/legacyImportedV1").arg(kProfileId)).toBool());
}

void TestChatHistory::open_whenHistoryExists_shouldRestrictPermissionsToOwner() {
#ifndef Q_OS_UNIX
    QSKIP("Owner permission bits are not portable on this platform.");
#else
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const ProfileChatStoreOptions options = optionsFor(directory);
    const QString path = historyPath(options);
    QVERIFY(QDir().mkpath(QFileInfo(path).path()));
    QFile history(path);
    QVERIFY(history.open(QIODevice::WriteOnly));
    history.close();
    QVERIFY(QFile::setPermissions(
        path, QFileDevice::ReadOwner | QFileDevice::WriteOwner
            | QFileDevice::ReadGroup | QFileDevice::WriteGroup
            | QFileDevice::ReadOther | QFileDevice::WriteOther));

    ProfileChatHistoryStore store;
    QString error;
    QVERIFY2(store.open(options, &error), qPrintable(error));

    const QFileDevice::Permissions permissions = QFile::permissions(path);
    QVERIFY(permissions.testFlag(QFileDevice::ReadOwner));
    QVERIFY(permissions.testFlag(QFileDevice::WriteOwner));
    QCOMPARE(permissions
                 & (QFileDevice::ReadGroup | QFileDevice::WriteGroup
                    | QFileDevice::ExeGroup | QFileDevice::ReadOther
                    | QFileDevice::WriteOther | QFileDevice::ExeOther),
             QFileDevice::Permissions{});
#endif
}

void TestChatHistory::load_whenLineIsCorruptOversizedOrPartial_shouldSkipItAndContinue() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const ProfileChatStoreOptions options = optionsFor(directory);
    ProfileChatHistoryStore store;
    QString error;
    QVERIFY2(store.open(options, &error), qPrintable(error));

    QFile file(historyPath(options));
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(encodedLine(entry(QStringLiteral("one"), QStringLiteral("user"),
                                 QStringLiteral("first"))) + '\n');
    file.write("not-json\n");
    file.write(QByteArray(1024 * 1024 + 1, 'x') + '\n');
    file.write(encodedLine(entry(QStringLiteral("two"), QStringLiteral("assistant"),
                                 QStringLiteral("second"))) + '\n');
    file.write(encodedLine(entry(QStringLiteral("partial"), QStringLiteral("user"),
                                 QStringLiteral("ignored"))));
    file.close();

    const QList<ChatHistoryEntry> loaded = store.load(&error);
    QCOMPARE(loaded.size(), 2);
    QCOMPARE(loaded.at(0).id, QStringLiteral("one"));
    QCOMPARE(loaded.at(1).id, QStringLiteral("two"));
}

void TestChatHistory::load_whenReplyToIdIsMissingInLegacyEntry_shouldUseEmptyValue() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const ProfileChatStoreOptions options = optionsFor(directory);
    ProfileChatHistoryStore store;
    QString error;
    QVERIFY2(store.open(options, &error), qPrintable(error));
    QFile file(historyPath(options));
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(encodedLine(entry(QStringLiteral("assistant"), QStringLiteral("assistant"),
                                 QStringLiteral("hello")), false) + '\n');
    file.close();

    const QList<ChatHistoryEntry> loaded = store.load(&error);
    QCOMPARE(loaded.size(), 1);
    QVERIFY(loaded.first().replyToId.isEmpty());
}

void TestChatHistory::load_whenStatusIsInvalidOrIdDuplicated_shouldKeepFirstValidTerminalEntry() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const ProfileChatStoreOptions options = optionsFor(directory);
    ProfileChatHistoryStore store;
    QString error;
    QVERIFY2(store.open(options, &error), qPrintable(error));
    const ChatHistoryEntry first = entry(
        QStringLiteral("duplicate"), QStringLiteral("assistant"), QStringLiteral("first valid"));
    ChatHistoryEntry second = first;
    second.content = QStringLiteral("second valid");
    QJsonObject invalidStatus = QJsonDocument::fromJson(encodedLine(first)).object();
    invalidStatus.insert(QStringLiteral("status"), QStringLiteral("streaming"));
    QFile file(historyPath(options));
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QJsonDocument(invalidStatus).toJson(QJsonDocument::Compact) + '\n');
    file.write(encodedLine(first) + '\n');
    file.write(encodedLine(second) + '\n');
    file.close();

    const QList<ChatHistoryEntry> loaded = store.load(&error);
    QCOMPARE(loaded.size(), 1);
    QCOMPARE(loaded.first().id, QStringLiteral("duplicate"));
    QCOMPARE(loaded.first().content, QStringLiteral("first valid"));
}

void TestChatHistory::appendFinal_whenEntryIsValid_shouldRoundTripSchemaVersionTwoFields() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const ProfileChatStoreOptions options = optionsFor(directory);
    ProfileChatHistoryStore store;
    QString error;
    QVERIFY2(store.open(options, &error), qPrintable(error));
    const ChatHistoryEntry expected = entry(
        QStringLiteral("assistant-id"), QStringLiteral("assistant"), QStringLiteral("answer"));
    QVERIFY2(store.appendFinal(expected, &error), qPrintable(error));

    const QList<ChatHistoryEntry> loaded = store.load(&error);
    QCOMPARE(loaded.size(), 1);
    QCOMPARE(loaded.first().id, expected.id);
    QCOMPARE(loaded.first().role, expected.role);
    QCOMPARE(loaded.first().content, expected.content);
    QCOMPARE(loaded.first().timestamp, expected.timestamp);
    QCOMPARE(loaded.first().status, ChatMessageStatus::Complete);
    QFile file(historyPath(options));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject object = QJsonDocument::fromJson(file.readLine()).object();
    QCOMPARE(object.value(QStringLiteral("schemaVersion")).toInt(), 2);
    QCOMPARE(object.value(QStringLiteral("status")).toString(), QStringLiteral("complete"));
}

void TestChatHistory::appendFinal_whenAssistantHasReplyToId_shouldRoundTripReplyLink() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const ProfileChatStoreOptions options = optionsFor(directory);
    ProfileChatHistoryStore store;
    QString error;
    QVERIFY2(store.open(options, &error), qPrintable(error));
    ChatHistoryEntry expected = entry(
        QStringLiteral("assistant-id"), QStringLiteral("assistant"), QStringLiteral("answer"));
    expected.replyToId = QStringLiteral("user-id");
    QVERIFY2(store.appendFinal(expected, &error), qPrintable(error));

    const QList<ChatHistoryEntry> loaded = store.load(&error);
    QCOMPARE(loaded.size(), 1);
    QCOMPARE(loaded.first().replyToId, QStringLiteral("user-id"));
}

void TestChatHistory::appendFinal_whenHistoryEndsWithPartialLine_shouldWriteIndependentValidLine() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const ProfileChatStoreOptions options = optionsFor(directory);
    ProfileChatHistoryStore store;
    QString error;
    QVERIFY2(store.open(options, &error), qPrintable(error));
    QFile file(historyPath(options));
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(file.write("{\"schemaVersion\":2,\"id\":\"partial\"") > 0);
    file.close();

    const ChatHistoryEntry expected = entry(
        QStringLiteral("new-entry"), QStringLiteral("assistant"), QStringLiteral("survives"));
    QVERIFY2(store.appendFinal(expected, &error), qPrintable(error));

    const QList<ChatHistoryEntry> loaded = store.load(&error);
    QCOMPARE(loaded.size(), 1);
    QCOMPARE(loaded.first().id, expected.id);
    QCOMPARE(loaded.first().content, expected.content);
}

void TestChatHistory::appendFinal_whenEntryIsNonTerminal_shouldRejectWithoutWriting() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const ProfileChatStoreOptions options = optionsFor(directory);
    ProfileChatHistoryStore store;
    QString error;
    QVERIFY2(store.open(options, &error), qPrintable(error));

    QVERIFY(!store.appendFinal(entry(QStringLiteral("pending"), QStringLiteral("assistant"),
                                     QString(), ChatMessageStatus::Pending), &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(nonEmptyLineCount(historyPath(options)), 0);
}

void TestChatHistory::appendFinal_whenNonAssistantHasReplyToId_shouldRejectWithoutWriting() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const ProfileChatStoreOptions options = optionsFor(directory);
    ProfileChatHistoryStore store;
    QString error;
    QVERIFY2(store.open(options, &error), qPrintable(error));
    ChatHistoryEntry invalid = entry(
        QStringLiteral("user"), QStringLiteral("user"), QStringLiteral("question"));
    invalid.replyToId = QStringLiteral("another-user");

    QVERIFY(!store.appendFinal(invalid, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(nonEmptyLineCount(historyPath(options)), 0);
}

void TestChatHistory::v2History_importsOnceAndWritesOnlySqliteAcrossThreadCopies() {
    QTemporaryDir directory;auto options=optionsFor(directory);QString error;
    ProfileChatHistoryStore legacy;QVERIFY(legacy.open(options,&error));
    auto first=entry("old-user","user","original");QVERIFY(legacy.appendFinal(first,&error));
    auto interrupted=entry("old-assistant","assistant","partial",ChatMessageStatus::Interrupted);
    interrupted.replyToId=first.id;interrupted.sourceMessageIds={first.id};QVERIFY(legacy.appendFinal(interrupted,&error));
    options.runtimeDatabasePath=QDir(options.appDataRoot).filePath(QStringLiteral("profiles/%1/agent_runtime.sqlite").arg(options.profileId));
    ProfileChatHistoryStore store;QVERIFY2(store.open(options,&error),qPrintable(error));
    const auto loaded=store.load(&error);QCOMPARE(loaded.size(),2);QCOMPARE(loaded.last().status,ChatMessageStatus::Interrupted);
    QCOMPARE(loaded.last().replyToId,first.id);
    std::atomic_bool done=false;QString threadError;
    QThreadPool::globalInstance()->start([copy=store,&done,&threadError]()mutable{
        copy.appendFinal(entry("new-user","user","only sqlite"),&threadError);done=true;
    });
    QTRY_VERIFY(done.load());QVERIFY2(threadError.isEmpty(),qPrintable(threadError));
    QCOMPARE(store.load(&error).size(),3);QCOMPARE(nonEmptyLineCount(historyPath(options)),2);
    // Once SQLite is authoritative, an old writer cannot inject new history on reopen.
    QVERIFY(legacy.appendFinal(entry("injected-jsonl","user","do not import"),&error));
    ProfileChatHistoryStore reopened;QVERIFY(reopened.open(options,&error));QCOMPARE(reopened.load(&error).size(),3);
}

void TestChatHistory::historyMigration_v2Failure_neverFallsBackToJsonlWrites() {
    QTemporaryDir directory;
    auto options = optionsFor(directory);
    QString error;
    ProfileChatHistoryStore legacy;
    QVERIFY(legacy.open(options, &error));
    QVERIFY(legacy.appendFinal(entry("original", "user", "preserve this file"), &error));
    QFile file(historyPath(options));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto originalBytes = file.readAll();
    file.close();
    options.runtimeDatabasePath = directory.filePath("runtime.sqlite");
    ProfileChatHistoryStore store;
    QVERIFY2(store.open(options, &error), qPrintable(error));
    {
        auto unit = SqliteRuntimeUnitOfWork::begin(options.runtimeDatabasePath);
        QVERIFY(unit);
        QSqlQuery query(QSqlDatabase::database(unit.value()->connectionName()));
        QVERIFY(query.exec("CREATE TRIGGER fail_history BEFORE INSERT ON agent_messages BEGIN SELECT RAISE(ABORT,'injected write failure'); END"));
        QVERIFY(unit.value()->commit());
    }
    QVERIFY(!store.appendFinal(entry("new", "user", "must not fall back"), &error));
    QVERIFY(error.contains("injected write failure"));
    QCOMPARE(store.load(&error).size(), 1);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), originalBytes);
}

void TestChatHistory::historyMigration_completedSqlite_doesNotRequireLegacyFile() {
    QTemporaryDir directory;
    auto options = optionsFor(directory);
    options.runtimeDatabasePath = directory.filePath("runtime.sqlite");
    QString error;
    ProfileChatHistoryStore store;
    QVERIFY(store.open(options, &error));
    QVERIFY(store.appendFinal(entry("live", "user", "authoritative SQLite"), &error));
    QVERIFY(QFile::remove(historyPath(options)));
    // Replace the source with a directory: neither reading nor recreating it is valid.
    QVERIFY(QDir().mkpath(historyPath(options)));
    ProfileChatHistoryStore reopened;
    QVERIFY2(reopened.open(options, &error), qPrintable(error));
    const auto history = reopened.load(&error);
    QCOMPARE(history.size(), 1);
    QCOMPARE(history.first().id, QString("live"));
}

void TestChatHistory::historyMigration_sourceReadFailure_doesNotCompleteEmptyImport() {
    QTemporaryDir directory;
    auto options = optionsFor(directory);
    options.runtimeDatabasePath = directory.filePath("runtime.sqlite");
    QVERIFY(QDir().mkpath(historyPath(options)));
    QString error;
    ProfileChatHistoryStore store;
    QVERIFY(!store.open(options, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(QDir(historyPath(options)).removeRecursively());
    QFile file(historyPath(options));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(encodedLine(entry("recoverable", "user", "read after repair")) + '\n');
    file.close();
    QVERIFY2(store.open(options, &error), qPrintable(error));
    QCOMPARE(store.load(&error).size(), 1);
}


void TestChatHistory::queryHistory_data() {
    QTest::addColumn<bool>("sqlite");
    QTest::newRow("jsonl") << false;
    QTest::newRow("sqlite") << true;
}

void TestChatHistory::queryHistory() {
    QFETCH(bool, sqlite);
    QTemporaryDir directory;
    auto options = optionsFor(directory);
    if (sqlite) options.runtimeDatabasePath = directory.filePath("runtime.sqlite");
    ProfileChatHistoryStore store;
    QString error;
    QVERIFY2(store.open(options, &error), qPrintable(error));
    auto empty = store.queryHistory({});
    QVERIFY(empty);
    QVERIFY(empty.value()["messages"].toArray().isEmpty());
    QVERIFY(!empty.value()["hasMoreBefore"].toBool());
    for (int i = 0; i < 185; ++i) {
        QVERIFY(store.appendFinal(entry(QString::number(i), i % 2 ? "assistant" : "user",
            i % 3 ? QString("合成消息 %1").arg(i) : QString("中文 100%_ TeSt %1").arg(i)), &error));
    }
    // Diagnostics and empty failures must not consume a visible history page.
    QVERIFY(store.appendFinal(entry("internal", "system", "中文 100%_ test internal"), &error));
    QVERIFY(store.appendFinal(entry("empty-failure", "assistant", "", ChatMessageStatus::Failed), &error));
    QStringList all;
    QJsonObject request{{"limit", 40}, {"requestToken", "page-token"}};
    bool more = true;
    while (more) {
        auto page = store.queryHistory(request);
        QVERIFY2(page, page ? "" : qPrintable(page.error().message));
        QCOMPARE(page.value()["requestToken"].toString(), QString("page-token"));
        const auto messages = page.value()["messages"].toArray();
        QVERIFY(messages.size() <= 40);
        QStringList ids;
        for (const auto& message : messages) ids.append(message.toObject()["id"].toString());
        all = ids + all;
        request["beforeMessageId"] = page.value()["oldestMessageId"];
        more = page.value()["hasMoreBefore"].toBool();
        if (all.size() == 40) {
            QVERIFY(store.appendFinal(entry("new", "user", "later append"), &error));
        }
    }
    QCOMPARE(all.size(), 185);
    for (int i = 0; i < all.size(); ++i) QCOMPARE(all[i], QString::number(i));

    auto search = store.queryHistory({{"query", "  中文 100%_ test  "}, {"limit", 30}});
    QVERIFY(search);
    QCOMPARE(search.value()["query"].toString(), QString("中文 100%_ test"));
    QCOMPARE(search.value()["messages"].toArray().size(), 30);
    QCOMPARE(search.value()["messages"].toArray()[0].toObject()["id"].toString(), QString("183"));
    auto search2 = store.queryHistory({{"query", "中文 100%_ test"}, {"limit", 50},
        {"beforeMessageId", search.value()["nextBeforeMessageId"]}});
    QVERIFY(search2);
    QCOMPARE(search2.value()["messages"].toArray().size(), 32);
    QVERIFY(!search2.value()["hasMoreBefore"].toBool());
    QCOMPARE(search2.value()["messages"].toArray().last().toObject()["id"].toString(), QString("0"));
    auto noMatch = store.queryHistory({{"query", "找不到"}});
    QVERIFY(noMatch);
    QVERIFY(noMatch.value()["messages"].toArray().isEmpty());

    auto around = store.queryHistory({{"aroundMessageId", "75"}, {"limit", 5}});
    QVERIFY(around);
    QCOMPARE(around.value()["anchorMessageId"].toString(), QString("75"));
    QCOMPARE(around.value()["messages"].toArray().size(), 5);
    QCOMPARE(around.value()["messages"].toArray()[2].toObject()["id"].toString(), QString("75"));
    QVERIFY(around.value()["hasMoreBefore"].toBool());
    QVERIFY(around.value()["hasMoreAfter"].toBool());
    for (const auto& invalid : QList<QJsonObject>{
        {{"beforeMessageId", "missing"}}, {{"aroundMessageId", "missing"}},
        {{"beforeMessageId", "internal"}}, {{"aroundMessageId", "empty-failure"}},
        {{"beforeMessageId", "4"}, {"aroundMessageId", "5"}},
        {{"query", "   "}}, {{"query", QString(201, 'x')}}, {{"limit", 81}},
        {{"query", "x"}, {"limit", 51}}, {{"limit", 1.5}}, {{"limit", "40"}},
        {{"profileId", kOtherProfileId}}, {{"conversationId", "external"}}
    }) {
        auto result = store.queryHistory(invalid);
        QVERIFY(!result);
        QCOMPARE(result.error().code, QString("CHAT_QUERY_INVALID"));
    }
    if (sqlite) QVERIFY(QFile::remove(options.runtimeDatabasePath));
    else QVERIFY(QFile::remove(historyPath(options)));
    auto unavailable = store.queryHistory({});
    QVERIFY(!unavailable);
    QCOMPARE(unavailable.error().code, QString("STORE_UNAVAILABLE"));
}

void TestChatHistory::queryHistory_sqliteFullArchiveAndScope() {
    QTemporaryDir directory;
    const QString path = directory.filePath("runtime.sqlite");
    ConversationStore store(kProfileId, path), other(kOtherProfileId, path);
    QVERIFY(store.initialize());
    QVERIFY(other.initialize());
    {
        auto transaction = SqliteRuntimeUnitOfWork::begin(path);
        QVERIFY(transaction);
        auto db = QSqlDatabase::database(transaction.value()->connectionName());
        QSqlQuery insert(db);
        insert.prepare("INSERT INTO agent_messages(profile_id,id,conversation_id,direction,payload_json,occurred_at) VALUES(?,?,'local_owner','incoming',?,?)");
        for (int i = 0; i < 10125; ++i) {
            insert.bindValue(0, kProfileId);
            insert.bindValue(1, QString("archive-%1").arg(i));
            insert.bindValue(2, QString::fromUtf8(QJsonDocument(QJsonObject{{"role", "user"}, {"text", QString("archive %1").arg(i)}}).toJson(QJsonDocument::Compact)));
            insert.bindValue(3, "2026-10-18T01:00:00.000Z");
            QVERIFY(insert.exec());
        }
        QVERIFY(transaction.value()->commit());
    }
    QVERIFY(other.appendLocalHistory(entry("other-only", "user", "private other")));
    {
        auto transaction = SqliteRuntimeUnitOfWork::begin(path);
        QVERIFY(transaction);
        auto db = QSqlDatabase::database(transaction.value()->connectionName());
        QSqlQuery q(db);
        q.prepare("INSERT INTO agent_conversations(profile_id,id,channel,connection_key,remote_id,kind) VALUES(?,'external','test','test','test','direct')");
        q.addBindValue(kProfileId);
        QVERIFY(q.exec());
        q.prepare("INSERT INTO agent_messages(profile_id,id,conversation_id,direction,payload_json) VALUES(?,'external-only','external','incoming','{\"role\":\"user\",\"text\":\"private external\"}')");
        q.addBindValue(kProfileId);
        QVERIFY(q.exec());
        QVERIFY(transaction.value()->commit());
    }
    auto earliest = store.queryLocalHistory({{"query", "archive 0"}});
    QVERIFY(earliest);
    QCOMPARE(earliest.value()["messages"].toArray().size(), 1);
    QCOMPARE(earliest.value()["messages"].toArray()[0].toObject()["id"].toString(), QString("archive-0"));
    auto privateSearch = store.queryLocalHistory({{"query", "private"}});
    QVERIFY(privateSearch);
    QVERIFY(privateSearch.value()["messages"].toArray().isEmpty());
    for (const auto& id : {QString("other-only"), QString("external-only")}) {
        auto denied = store.queryLocalHistory({{"aroundMessageId", id}});
        QVERIFY(!denied);
        QCOMPARE(denied.error().code, QString("CHAT_QUERY_INVALID"));
    }
}

QTEST_GUILESS_MAIN(TestChatHistory)
#include "test_chat_history.moc"
