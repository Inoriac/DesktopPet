#include <QtTest>

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>

#include <memory>
#include <optional>

#include "ai/chat/profile_chat_history_store.h"

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

QTEST_GUILESS_MAIN(TestChatHistory)
#include "test_chat_history.moc"
