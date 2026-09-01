#include <QtTest>

#include <QFile>
#include <QDir>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <memory>

#include "ui/chat_conversation_model.h"

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
    options.legacyHistoryPath = directory.filePath(QStringLiteral("missing.jsonl"));
    return options;
}

QString historyPath(const ProfileChatStoreOptions& options) {
    return QDir(options.appDataRoot).filePath(
        QStringLiteral("profiles/%1/chat_history.jsonl").arg(options.profileId));
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

class TestChatConversationModel : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();
    void appendUserMessage_whenTextIsValid_shouldInsertPersistAndReturnStableId();
    void appendUserMessage_whenTextIsBlank_shouldRejectWithoutMutation();
    void initialize_whenHistoryCannotOpen_shouldKeepInMemoryConversationUsable();
    void assistantLifecycle_whenDeltasComplete_shouldPersistOneJoinedTerminalEntry();
    void assistantLifecycle_whenToolContinuationUsesSameId_shouldKeepReplyToIdAndOneEntry();
    void beginAssistantMessage_whenReplyToIdIsInvalid_shouldRejectWithoutInsertion();
    void finishAssistantMessage_whenCalledTwice_shouldPersistOnlyOnce();
    void finishAssistantMessage_whenFailed_shouldPersistSanitizedErrorSeparately();
    void markReadThrough_whenMessageExists_shouldRestoreProfileScopedMarker();
    void markReadThrough_whenMessageIsUnknown_shouldLeaveMarkerUnchanged();
    void initialize_whenLastReadMarkerIsStale_shouldReturnEmptyMarker();

private:
    std::unique_ptr<QTemporaryDir> m_settingsDirectory;
};

void TestChatConversationModel::initTestCase() {
    m_settingsDirectory = std::make_unique<QTemporaryDir>();
    QVERIFY(m_settingsDirectory->isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       m_settingsDirectory->path());
    QCoreApplication::setOrganizationName(QStringLiteral("Desktop Pet Team Test"));
    QCoreApplication::setApplicationName(QStringLiteral("Chat Model Tests"));
}

void TestChatConversationModel::cleanup() {
    QSettings settings;
    settings.clear();
    settings.sync();
}

void TestChatConversationModel::appendUserMessage_whenTextIsValid_shouldInsertPersistAndReturnStableId() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const ProfileChatStoreOptions options = optionsFor(directory);
    ChatConversationModel model;
    QString error;
    QVERIFY2(model.initialize(options, &error), qPrintable(error));
    QSignalSpy inserted(&model, &ChatConversationModel::messageInserted);
    const QDateTime timestamp = QDateTime::fromString(
        QStringLiteral("2026-08-27T12:00:00.000+08:00"), Qt::ISODateWithMs);

    const QString id = model.appendUserMessage(QStringLiteral("  hello\nworld  "), timestamp);

    QVERIFY(!id.isEmpty());
    QCOMPARE(inserted.count(), 1);
    QCOMPARE(inserted.first().at(1).toString(), id);
    QCOMPARE(model.messages().size(), 1);
    QCOMPARE(model.messages().first().id, id);
    QCOMPARE(model.messages().first().content, QStringLiteral("hello\nworld"));
    QCOMPARE(model.messages().first().status, ChatMessageStatus::Complete);
    ProfileChatHistoryStore reloaded;
    QVERIFY2(reloaded.open(options, &error), qPrintable(error));
    QCOMPARE(reloaded.load(&error).first().id, id);
}

void TestChatConversationModel::appendUserMessage_whenTextIsBlank_shouldRejectWithoutMutation() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ChatConversationModel model;
    QString error;
    QVERIFY2(model.initialize(optionsFor(directory), &error), qPrintable(error));
    QSignalSpy inserted(&model, &ChatConversationModel::messageInserted);

    QVERIFY(model.appendUserMessage(QStringLiteral(" \n\t ")).isEmpty());
    QCOMPARE(model.messages().size(), 0);
    QCOMPARE(inserted.count(), 0);
}

void TestChatConversationModel::initialize_whenHistoryCannotOpen_shouldKeepInMemoryConversationUsable() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QFile blockedRoot(directory.filePath(QStringLiteral("blocked-root")));
    QVERIFY(blockedRoot.open(QIODevice::WriteOnly));
    blockedRoot.close();
    ProfileChatStoreOptions options = optionsFor(directory);
    options.appDataRoot = blockedRoot.fileName();
    ChatConversationModel model;
    QSignalSpy warnings(&model, &ChatConversationModel::historyPersistenceWarning);
    QString error;

    QVERIFY(model.initialize(options, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(warnings.count(), 1);
    const QString id = model.appendUserMessage(QStringLiteral("memory only"));
    QVERIFY(!id.isEmpty());
    QCOMPARE(model.messages().size(), 1);
    QCOMPARE(model.messages().first().content, QStringLiteral("memory only"));
}

void TestChatConversationModel::assistantLifecycle_whenDeltasComplete_shouldPersistOneJoinedTerminalEntry() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const ProfileChatStoreOptions options = optionsFor(directory);
    ChatConversationModel model;
    QString error;
    QVERIFY2(model.initialize(options, &error), qPrintable(error));
    QSignalSpy inserted(&model, &ChatConversationModel::messageInserted);
    QSignalSpy changed(&model, &ChatConversationModel::messageChanged);

    model.beginAssistantMessage(QStringLiteral("assistant-id"));
    model.appendAssistantDelta(QStringLiteral("assistant-id"), QStringLiteral("first"));
    model.appendAssistantDelta(QStringLiteral("assistant-id"), QStringLiteral(" second"));
    model.finishAssistantMessage(QStringLiteral("assistant-id"), ChatMessageStatus::Complete);

    QCOMPARE(inserted.count(), 1);
    QVERIFY(changed.count() >= 3);
    QCOMPARE(model.messages().size(), 1);
    QCOMPARE(model.messages().first().content, QStringLiteral("first second"));
    QCOMPARE(model.messages().first().status, ChatMessageStatus::Complete);
    QCOMPARE(nonEmptyLineCount(historyPath(options)), 1);
}

void TestChatConversationModel::assistantLifecycle_whenToolContinuationUsesSameId_shouldKeepReplyToIdAndOneEntry() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ChatConversationModel model;
    QString error;
    QVERIFY2(model.initialize(optionsFor(directory), &error), qPrintable(error));
    const QString userId = model.appendUserMessage(QStringLiteral("question"));

    model.beginAssistantMessage(QStringLiteral("assistant-id"), userId);
    model.appendAssistantDelta(QStringLiteral("assistant-id"), QStringLiteral("before tool"));
    model.setAssistantStage(QStringLiteral("assistant-id"), ChatActivityStage::PreparingTool);
    model.setAssistantStage(QStringLiteral("assistant-id"), ChatActivityStage::RunningTool);
    model.setAssistantStage(QStringLiteral("assistant-id"), ChatActivityStage::Finalizing);
    model.appendAssistantDelta(QStringLiteral("assistant-id"), QStringLiteral(" after tool"));
    model.finishAssistantMessage(QStringLiteral("assistant-id"), ChatMessageStatus::Complete);

    QCOMPARE(model.messages().size(), 2);
    const ChatHistoryEntry assistant = model.messages().last();
    QCOMPARE(assistant.id, QStringLiteral("assistant-id"));
    QCOMPARE(assistant.replyToId, userId);
    QCOMPARE(assistant.content, QStringLiteral("before tool after tool"));
}

void TestChatConversationModel::beginAssistantMessage_whenReplyToIdIsInvalid_shouldRejectWithoutInsertion() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ChatConversationModel model;
    QString error;
    QVERIFY2(model.initialize(optionsFor(directory), &error), qPrintable(error));
    QSignalSpy inserted(&model, &ChatConversationModel::messageInserted);
    QSignalSpy warnings(&model, &ChatConversationModel::historyPersistenceWarning);

    model.beginAssistantMessage(QStringLiteral("assistant-id"),
                                QStringLiteral("missing-user-id"));

    QVERIFY(model.messages().isEmpty());
    QCOMPARE(inserted.count(), 0);
    QCOMPARE(warnings.count(), 1);
}

void TestChatConversationModel::finishAssistantMessage_whenCalledTwice_shouldPersistOnlyOnce() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const ProfileChatStoreOptions options = optionsFor(directory);
    ChatConversationModel model;
    QString error;
    QVERIFY2(model.initialize(options, &error), qPrintable(error));
    QSignalSpy changed(&model, &ChatConversationModel::messageChanged);

    model.beginAssistantMessage(QStringLiteral("assistant-id"));
    model.appendAssistantDelta(QStringLiteral("assistant-id"), QStringLiteral("answer"));
    model.finishAssistantMessage(QStringLiteral("assistant-id"), ChatMessageStatus::Complete);
    const int changesAfterFirstFinish = changed.count();
    model.finishAssistantMessage(QStringLiteral("assistant-id"), ChatMessageStatus::Failed,
                                 QStringLiteral("late failure"));

    QCOMPARE(changed.count(), changesAfterFirstFinish);
    QCOMPARE(model.messages().first().status, ChatMessageStatus::Complete);
    QCOMPARE(nonEmptyLineCount(historyPath(options)), 1);
}

void TestChatConversationModel::finishAssistantMessage_whenFailed_shouldPersistSanitizedErrorSeparately() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const ProfileChatStoreOptions options = optionsFor(directory);
    ChatConversationModel model;
    QString error;
    QVERIFY2(model.initialize(options, &error), qPrintable(error));

    model.beginAssistantMessage(QStringLiteral("assistant-id"));
    model.finishAssistantMessage(
        QStringLiteral("assistant-id"), ChatMessageStatus::Failed,
        QStringLiteral("LLM HTTP error (HTTP 400): api-key=super-secret"));

    const ChatHistoryEntry failed = model.messages().first();
    QCOMPARE(failed.status, ChatMessageStatus::Failed);
    QVERIFY(failed.content.isEmpty());
    QVERIFY(failed.errorMessage.contains(QStringLiteral("HTTP 400")));
    QVERIFY(failed.errorMessage.contains(QStringLiteral("[REDACTED]")));
    QVERIFY(!failed.errorMessage.contains(QStringLiteral("super-secret")));

    ProfileChatHistoryStore reloaded;
    QVERIFY2(reloaded.open(options, &error), qPrintable(error));
    const ChatHistoryEntry persisted = reloaded.load(&error).first();
    QCOMPARE(persisted.errorMessage, failed.errorMessage);
}

void TestChatConversationModel::markReadThrough_whenMessageExists_shouldRestoreProfileScopedMarker() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const ProfileChatStoreOptions options = optionsFor(directory);
    ChatConversationModel model;
    QString error;
    QVERIFY2(model.initialize(options, &error), qPrintable(error));
    const QString firstId = model.appendUserMessage(QStringLiteral("first"));
    model.appendUserMessage(QStringLiteral("second"));
    model.markReadThrough(firstId);

    ChatConversationModel restored;
    QVERIFY2(restored.initialize(options, &error), qPrintable(error));
    QCOMPARE(restored.lastReadMessageId(), firstId);

    ChatConversationModel otherProfile;
    QVERIFY2(otherProfile.initialize(optionsFor(directory, kOtherProfileId), &error),
             qPrintable(error));
    QVERIFY(otherProfile.lastReadMessageId().isEmpty());
}

void TestChatConversationModel::markReadThrough_whenMessageIsUnknown_shouldLeaveMarkerUnchanged() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ChatConversationModel model;
    QString error;
    QVERIFY2(model.initialize(optionsFor(directory), &error), qPrintable(error));
    const QString knownId = model.appendUserMessage(QStringLiteral("known"));
    model.markReadThrough(knownId);

    model.markReadThrough(QStringLiteral("unknown"));

    QCOMPARE(model.lastReadMessageId(), knownId);
}

void TestChatConversationModel::initialize_whenLastReadMarkerIsStale_shouldReturnEmptyMarker() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const ProfileChatStoreOptions options = optionsFor(directory);
    QSettings().setValue(
        QStringLiteral("chat/%1/lastReadMessageId").arg(kProfileId),
        QStringLiteral("missing-message"));
    ChatConversationModel model;
    QString error;

    QVERIFY2(model.initialize(options, &error), qPrintable(error));
    QVERIFY(model.lastReadMessageId().isEmpty());
}

QTEST_GUILESS_MAIN(TestChatConversationModel)
#include "test_chat_conversation_model.moc"
