#include <QtTest>

#include <QApplication>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QVBoxLayout>

#include <memory>

#include "ui/chat_conversation_model.h"
#include "ui/chat_history_window.h"

namespace {

const QString kProfileId =
    QStringLiteral("5bb00e6d-937a-4f46-9c87-e3933c078f5a");

ProfileChatStoreOptions optionsFor(const QTemporaryDir& directory) {
    ProfileChatStoreOptions options;
    options.appDataRoot = directory.filePath(QStringLiteral("app-data"));
    options.profileId = kProfileId;
    options.registeredProfileIds = {kProfileId};
    options.legacyHistoryPath = directory.filePath(QStringLiteral("missing.jsonl"));
    return options;
}

void initializeModel(ChatConversationModel& model,
                     const QTemporaryDir& directory) {
    QString error;
    QVERIFY2(model.initialize(optionsFor(directory), &error), qPrintable(error));
}

QWidget* messageRow(ChatHistoryWindow& window, const QString& messageId) {
    const QList<QWidget*> rows = window.findChildren<QWidget*>(
        QStringLiteral("chatMessageRow"));
    for (QWidget* row : rows) {
        if (row->property("messageId").toString() == messageId) return row;
    }
    return nullptr;
}

QPushButton* retryButton(QWidget* row) {
    return row ? row->findChild<QPushButton*>(QStringLiteral("retryButton"))
               : nullptr;
}

void appendAssistant(ChatConversationModel& model,
                     const QString& id,
                     const QString& text,
                     const QString& replyToId = {},
                     ChatMessageStatus status = ChatMessageStatus::Complete) {
    model.beginAssistantMessage(id, replyToId);
    model.appendAssistantDelta(id, text);
    model.finishAssistantMessage(id, status);
}

void populateScrollableConversation(ChatConversationModel& model) {
    for (int index = 0; index < 14; ++index) {
        model.appendUserMessage(
            QStringLiteral("message %1 with enough text to occupy a visible row")
                .arg(index));
    }
}

} // namespace

class TestChatHistoryWindow : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();
    void bindConversation_whenModelHasMixedRoles_shouldRenderOrderedSelectableRowsWithoutConversationList();
    void bindConversation_whenModelChangesOneMessage_shouldUpdateOnlyThatRow();
    void bindConversation_whenHistoryIsLarge_shouldHydrateRowsInBatches();
    void revealConversation_whenUnreadMessagesExist_shouldInsertOneVirtualLastReadDivider();
    void revealConversation_whenSavedGeometryIsOffscreen_shouldClampToAvailableScreen();
    void revealConversation_whenWindowIsAlreadyVisible_shouldKeepCurrentGeometry();
    void streamingUpdate_whenViewportIsAtBottom_shouldFollowNewestDelta();
    void streamingUpdate_whenUserHasScrolledUp_shouldPreserveScrollAndShowJumpButton();
    void terminalUpdate_whenStopped_shouldFlushImmediatelyAndPreserveScrolledViewport();
    void setHeightRange_whenDocumentGrows_shouldClampHeightAndUseInternalScroll();
    void messageSubmitted_whenIdleAndEnterPressed_shouldEmitTrimmedTextAndClearInput();
    void stopRequested_whenResponseIsActive_shouldKeepDraftAndEmitStop();
    void retryRequested_whenInterruptedReplyHasSourceUser_shouldEmitAssistantIdWithoutMutatingOldMessage();
    void retryRequested_whenReplyToIdIsMissing_shouldHideRetryAction();
    void retryRequested_whenAnotherResponseBecomesActive_shouldDisableUntilTerminal();
    void lastFullyVisibleMessageId_whenLastRowIsPartial_shouldReturnPreviousCompleteRow();

private:
    std::unique_ptr<QTemporaryDir> m_settingsDirectory;
};

void TestChatHistoryWindow::initTestCase() {
    m_settingsDirectory = std::make_unique<QTemporaryDir>();
    QVERIFY(m_settingsDirectory->isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       m_settingsDirectory->path());
    QCoreApplication::setOrganizationName(
        QStringLiteral("Desktop Pet Team Test"));
    QCoreApplication::setApplicationName(
        QStringLiteral("Chat History Window Tests"));
}

void TestChatHistoryWindow::cleanup() {
    QSettings settings;
    settings.clear();
    settings.sync();
}

void TestChatHistoryWindow::bindConversation_whenModelHasMixedRoles_shouldRenderOrderedSelectableRowsWithoutConversationList() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const ProfileChatStoreOptions options = optionsFor(directory);
    ProfileChatHistoryStore store;
    QString error;
    QVERIFY2(store.open(options, &error), qPrintable(error));
    const QDateTime timestamp = QDateTime::currentDateTime();
    for (const ChatHistoryEntry& entry : {
             ChatHistoryEntry{QStringLiteral("user-id"), QStringLiteral("user"), {},
                              QStringLiteral("hello"), timestamp,
                              ChatMessageStatus::Complete},
             ChatHistoryEntry{QStringLiteral("assistant-id"), QStringLiteral("assistant"),
                              QStringLiteral("user-id"), QStringLiteral("hi"), timestamp,
                              ChatMessageStatus::Complete},
             ChatHistoryEntry{QStringLiteral("system-id"), QStringLiteral("system"), {},
                              QStringLiteral("notice"), timestamp,
                              ChatMessageStatus::Complete}}) {
        QVERIFY2(store.appendFinal(entry, &error), qPrintable(error));
    }
    ChatConversationModel model;
    QVERIFY2(model.initialize(options, &error), qPrintable(error));
    ChatHistoryWindow window;

    window.bindConversation(&model, kProfileId, QStringLiteral("Mochi"));

    const QList<QWidget*> rows = window.findChildren<QWidget*>(
        QStringLiteral("chatMessageRow"));
    QCOMPARE(rows.size(), 3);
    QCOMPARE(rows.at(0)->property("messageId").toString(),
             QStringLiteral("user-id"));
    QCOMPARE(rows.at(1)->property("messageId").toString(),
             QStringLiteral("assistant-id"));
    QCOMPARE(rows.at(2)->property("messageId").toString(),
             QStringLiteral("system-id"));
    QLabel* assistantBody = rows.at(1)->findChild<QLabel*>(
        QStringLiteral("messageBody"));
    QVERIFY(assistantBody);
    QVERIFY(assistantBody->textInteractionFlags().testFlag(
        Qt::TextSelectableByMouse));
    QVERIFY(!window.findChild<QWidget*>(QStringLiteral("conversationList")));
}

void TestChatHistoryWindow::bindConversation_whenModelChangesOneMessage_shouldUpdateOnlyThatRow() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ChatConversationModel model;
    initializeModel(model, directory);
    const QString userId = model.appendUserMessage(QStringLiteral("question"));
    model.beginAssistantMessage(QStringLiteral("assistant-id"), userId);
    model.appendAssistantDelta(QStringLiteral("assistant-id"), QStringLiteral("first"));
    ChatHistoryWindow window;
    window.bindConversation(&model, kProfileId, QStringLiteral("Mochi"));
    QWidget* userRow = messageRow(window, userId);
    QWidget* assistantRow = messageRow(window, QStringLiteral("assistant-id"));
    QVERIFY(userRow);
    QVERIFY(assistantRow);

    model.appendAssistantDelta(QStringLiteral("assistant-id"),
                               QStringLiteral(" second"));
    QTest::qWait(50);

    QCOMPARE(messageRow(window, userId), userRow);
    QCOMPARE(messageRow(window, QStringLiteral("assistant-id")), assistantRow);
    QCOMPARE(assistantRow->findChild<QLabel*>(QStringLiteral("messageBody"))->text(),
             QStringLiteral("first second"));
}

void TestChatHistoryWindow::bindConversation_whenHistoryIsLarge_shouldHydrateRowsInBatches() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ChatConversationModel model;
    initializeModel(model, directory);
    constexpr int messageCount = 30;
    for (int index = 0; index < messageCount; ++index) {
        model.appendUserMessage(QStringLiteral("message %1").arg(index));
    }
    ChatHistoryWindow window;

    window.bindConversation(&model, kProfileId, QStringLiteral("Mochi"));

    const int initialRows = window.findChildren<QWidget*>(
        QStringLiteral("chatMessageRow")).size();
    QVERIFY(initialRows > 0);
    QVERIFY(initialRows < messageCount);
    QTRY_COMPARE_WITH_TIMEOUT(
        window.findChildren<QWidget*>(QStringLiteral("chatMessageRow")).size(),
        messageCount, 1000);
}

void TestChatHistoryWindow::revealConversation_whenUnreadMessagesExist_shouldInsertOneVirtualLastReadDivider() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ChatConversationModel model;
    initializeModel(model, directory);
    const QString firstId = model.appendUserMessage(QStringLiteral("read"));
    model.markReadThrough(firstId);
    model.appendUserMessage(QStringLiteral("new one"));
    model.appendUserMessage(QStringLiteral("new two"));
    const int modelSize = model.messages().size();
    ChatHistoryWindow window;
    window.bindConversation(&model, kProfileId, QStringLiteral("Mochi"));

    window.revealConversation();
    QTest::qWait(30);

    QCOMPARE(window.findChildren<QWidget*>(QStringLiteral("lastReadDivider")).size(), 1);
    QCOMPARE(model.messages().size(), modelSize);
}

void TestChatHistoryWindow::revealConversation_whenSavedGeometryIsOffscreen_shouldClampToAvailableScreen() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ChatConversationModel model;
    initializeModel(model, directory);
    QWidget offscreen;
    offscreen.setGeometry(-100000, -100000, 460, 680);
    QSettings().setValue(
        QStringLiteral("chat/%1/windowGeometry").arg(kProfileId),
        offscreen.saveGeometry());
    ChatHistoryWindow window;
    window.bindConversation(&model, kProfileId, QStringLiteral("Mochi"));

    window.revealConversation();
    QTest::qWait(30);

    QScreen* screen = QGuiApplication::primaryScreen();
    QVERIFY(screen);
    QVERIFY(screen->availableGeometry().intersects(window.frameGeometry()));
    window.hide();
}

void TestChatHistoryWindow::revealConversation_whenWindowIsAlreadyVisible_shouldKeepCurrentGeometry() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ChatConversationModel model;
    initializeModel(model, directory);
    QScreen* screen = QGuiApplication::primaryScreen();
    QVERIFY(screen);
    QWidget savedWindow;
    const QRect available = screen->availableGeometry();
    savedWindow.setGeometry(
        available.left() + 40, available.top() + 40, 460, 680);
    QSettings().setValue(
        QStringLiteral("chat/%1/windowGeometry").arg(kProfileId),
        savedWindow.saveGeometry());
    ChatHistoryWindow window;
    window.bindConversation(&model, kProfileId, QStringLiteral("Mochi"));
    window.revealConversation();
    QTest::qWait(30);
    window.move(window.pos() + QPoint(24, 24));
    const QRect movedGeometry = window.geometry();

    window.revealConversation();
    QCoreApplication::processEvents();

    QCOMPARE(window.geometry(), movedGeometry);
    window.hide();
}

void TestChatHistoryWindow::streamingUpdate_whenViewportIsAtBottom_shouldFollowNewestDelta() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ChatConversationModel model;
    initializeModel(model, directory);
    populateScrollableConversation(model);
    ChatHistoryWindow window;
    window.resize(420, 520);
    window.bindConversation(&model, kProfileId, QStringLiteral("Mochi"));
    window.show();
    QTest::qWait(40);
    QScrollArea* area = window.findChild<QScrollArea*>(
        QStringLiteral("conversationScrollArea"));
    QVERIFY(area);
    QScrollBar* bar = area->verticalScrollBar();
    bar->setValue(bar->maximum());
    model.beginAssistantMessage(QStringLiteral("assistant-id"));
    QTest::qWait(40);
    bar->setValue(bar->maximum());

    model.appendAssistantDelta(
        QStringLiteral("assistant-id"),
        QString(600, QLatin1Char('x')));
    QTest::qWait(80);

    QCOMPARE(bar->value(), bar->maximum());
    window.hide();
}

void TestChatHistoryWindow::streamingUpdate_whenUserHasScrolledUp_shouldPreserveScrollAndShowJumpButton() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ChatConversationModel model;
    initializeModel(model, directory);
    populateScrollableConversation(model);
    model.beginAssistantMessage(QStringLiteral("assistant-id"));
    model.appendAssistantDelta(QStringLiteral("assistant-id"), QStringLiteral("start"));
    ChatHistoryWindow window;
    window.resize(420, 520);
    window.bindConversation(&model, kProfileId, QStringLiteral("Mochi"));
    window.show();
    QTest::qWait(40);
    QScrollArea* area = window.findChild<QScrollArea*>(
        QStringLiteral("conversationScrollArea"));
    QVERIFY(area);
    QScrollBar* bar = area->verticalScrollBar();
    QVERIFY(bar->maximum() > 0);
    bar->setValue(0);
    const int oldValue = bar->value();

    model.appendAssistantDelta(
        QStringLiteral("assistant-id"),
        QString(600, QLatin1Char('y')));
    QTest::qWait(80);

    QCOMPARE(bar->value(), oldValue);
    QPushButton* jump = window.findChild<QPushButton*>(
        QStringLiteral("jumpToLatestButton"));
    QVERIFY(jump);
    QVERIFY(jump->isVisible());
    window.hide();
}

void TestChatHistoryWindow::terminalUpdate_whenStopped_shouldFlushImmediatelyAndPreserveScrolledViewport() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ChatConversationModel model;
    initializeModel(model, directory);
    populateScrollableConversation(model);
    model.beginAssistantMessage(QStringLiteral("assistant-id"));
    ChatHistoryWindow window;
    window.resize(420, 520);
    window.bindConversation(&model, kProfileId, QStringLiteral("Mochi"));
    window.show();
    QTest::qWait(40);
    QScrollArea* area = window.findChild<QScrollArea*>(
        QStringLiteral("conversationScrollArea"));
    QPushButton* action = window.findChild<QPushButton*>(
        QStringLiteral("mainActionButton"));
    QWidget* row = messageRow(window, QStringLiteral("assistant-id"));
    QVERIFY(area);
    QVERIFY(action);
    QVERIFY(row);
    area->verticalScrollBar()->setValue(0);
    const int oldScrollValue = area->verticalScrollBar()->value();
    QCOMPARE(action->toolTip(), QStringLiteral("停止回复"));

    model.appendAssistantDelta(QStringLiteral("assistant-id"),
                               QStringLiteral("partial"));
    model.finishAssistantMessage(QStringLiteral("assistant-id"),
                                 ChatMessageStatus::Stopped);

    QCOMPARE(row->findChild<QLabel*>(QStringLiteral("messageBody"))->text(),
             QStringLiteral("partial"));
    QCOMPARE(row->findChild<QLabel*>(QStringLiteral("messageStatus"))->text(),
             QStringLiteral("已停止"));
    QCOMPARE(action->toolTip(), QStringLiteral("发送"));
    QCOMPARE(area->verticalScrollBar()->value(), oldScrollValue);
    window.hide();
}

void TestChatHistoryWindow::setHeightRange_whenDocumentGrows_shouldClampHeightAndUseInternalScroll() {
    GrowingPlainTextEdit edit;
    edit.resize(320, 44);
    edit.setHeightRange(44, 120);
    edit.show();
    edit.setPlainText(QStringLiteral("one line"));
    QTest::qWait(20);
    QVERIFY(edit.height() >= 44);
    QVERIFY(edit.height() <= 120);

    QStringList lines;
    for (int index = 0; index < 40; ++index) {
        lines.append(QStringLiteral("line %1").arg(index));
    }
    edit.setPlainText(lines.join(QLatin1Char('\n')));
    QTest::qWait(30);

    QCOMPARE(edit.height(), 120);
    QVERIFY(edit.verticalScrollBar()->maximum() > 0);
    edit.hide();
}

void TestChatHistoryWindow::messageSubmitted_whenIdleAndEnterPressed_shouldEmitTrimmedTextAndClearInput() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ChatConversationModel model;
    initializeModel(model, directory);
    ChatHistoryWindow window;
    window.bindConversation(&model, kProfileId, QStringLiteral("Mochi"));
    GrowingPlainTextEdit* input = window.findChild<GrowingPlainTextEdit*>(
        QStringLiteral("messageInput"));
    QVERIFY(input);
    QSignalSpy submitted(&window, &ChatHistoryWindow::messageSubmitted);
    input->setPlainText(QStringLiteral("  hello\nworld  "));
    input->setFocus();

    QTest::keyClick(input, Qt::Key_Return);

    QCOMPARE(submitted.count(), 1);
    QCOMPARE(submitted.first().first().toString(), QStringLiteral("hello\nworld"));
    QVERIFY(input->toPlainText().isEmpty());
}

void TestChatHistoryWindow::stopRequested_whenResponseIsActive_shouldKeepDraftAndEmitStop() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ChatConversationModel model;
    initializeModel(model, directory);
    model.beginAssistantMessage(QStringLiteral("assistant-id"));
    ChatHistoryWindow window;
    window.bindConversation(&model, kProfileId, QStringLiteral("Mochi"));
    GrowingPlainTextEdit* input = window.findChild<GrowingPlainTextEdit*>(
        QStringLiteral("messageInput"));
    QPushButton* action = window.findChild<QPushButton*>(
        QStringLiteral("mainActionButton"));
    QVERIFY(input);
    QVERIFY(action);
    input->setPlainText(QStringLiteral("keep this draft"));
    QSignalSpy stopped(&window, &ChatHistoryWindow::stopRequested);

    QTest::mouseClick(action, Qt::LeftButton);

    QCOMPARE(stopped.count(), 1);
    QCOMPARE(input->toPlainText(), QStringLiteral("keep this draft"));
}

void TestChatHistoryWindow::retryRequested_whenInterruptedReplyHasSourceUser_shouldEmitAssistantIdWithoutMutatingOldMessage() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ChatConversationModel model;
    initializeModel(model, directory);
    const QString userId = model.appendUserMessage(QStringLiteral("question"));
    appendAssistant(model, QStringLiteral("assistant-id"),
                    QStringLiteral("partial"), userId,
                    ChatMessageStatus::Interrupted);
    ChatHistoryWindow window;
    window.bindConversation(&model, kProfileId, QStringLiteral("Mochi"));
    QPushButton* retry = retryButton(
        messageRow(window, QStringLiteral("assistant-id")));
    QVERIFY(retry);
    QVERIFY(!retry->isHidden());
    QSignalSpy requested(&window, &ChatHistoryWindow::retryRequested);

    QTest::mouseClick(retry, Qt::LeftButton);

    QCOMPARE(requested.count(), 1);
    QCOMPARE(requested.first().first().toString(),
             QStringLiteral("assistant-id"));
    QCOMPARE(model.messages().last().status, ChatMessageStatus::Interrupted);
}

void TestChatHistoryWindow::retryRequested_whenReplyToIdIsMissing_shouldHideRetryAction() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ChatConversationModel model;
    initializeModel(model, directory);
    appendAssistant(model, QStringLiteral("assistant-id"),
                    QStringLiteral("partial"), {},
                    ChatMessageStatus::Interrupted);
    ChatHistoryWindow window;
    window.bindConversation(&model, kProfileId, QStringLiteral("Mochi"));

    QPushButton* retry = retryButton(
        messageRow(window, QStringLiteral("assistant-id")));
    QVERIFY(retry);
    QVERIFY(retry->isHidden());
}

void TestChatHistoryWindow::retryRequested_whenAnotherResponseBecomesActive_shouldDisableUntilTerminal() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ChatConversationModel model;
    initializeModel(model, directory);
    const QString userId = model.appendUserMessage(QStringLiteral("question"));
    appendAssistant(model, QStringLiteral("interrupted-id"),
                    QStringLiteral("partial"), userId,
                    ChatMessageStatus::Interrupted);
    ChatHistoryWindow window;
    window.bindConversation(&model, kProfileId, QStringLiteral("Mochi"));
    QPushButton* retry = retryButton(
        messageRow(window, QStringLiteral("interrupted-id")));
    QVERIFY(retry);
    QVERIFY(!retry->isHidden());
    QVERIFY(retry->isEnabled());
    QSignalSpy requested(&window, &ChatHistoryWindow::retryRequested);

    model.beginAssistantMessage(QStringLiteral("active-id"));

    QVERIFY(retry->isHidden() || !retry->isEnabled());
    retry->click();
    QCOMPARE(requested.count(), 0);
    QCOMPARE(model.messages().at(1).status, ChatMessageStatus::Interrupted);

    model.finishAssistantMessage(QStringLiteral("active-id"),
                                 ChatMessageStatus::Stopped);
    QVERIFY(!retry->isHidden());
    QVERIFY(retry->isEnabled());
    QCOMPARE(model.messages().at(1).status, ChatMessageStatus::Interrupted);
}

void TestChatHistoryWindow::lastFullyVisibleMessageId_whenLastRowIsPartial_shouldReturnPreviousCompleteRow() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ChatConversationModel model;
    initializeModel(model, directory);
    QStringList ids;
    for (int index = 0; index < 8; ++index) {
        ids.append(model.appendUserMessage(
            QStringLiteral("message %1 with a moderate amount of text").arg(index)));
    }
    ChatHistoryWindow window;
    window.resize(420, 520);
    window.bindConversation(&model, kProfileId, QStringLiteral("Mochi"));
    window.show();
    QTest::qWait(50);
    QScrollArea* area = window.findChild<QScrollArea*>(
        QStringLiteral("conversationScrollArea"));
    QWidget* lastRow = messageRow(window, ids.last());
    QWidget* previousRow = messageRow(window, ids.at(ids.size() - 2));
    QVERIFY(area);
    QVERIFY(lastRow);
    QVERIFY(previousRow);
    const int desiredViewportBottom =
        lastRow->geometry().bottom() - lastRow->height() / 2;
    area->verticalScrollBar()->setValue(
        qMax(0, desiredViewportBottom - area->viewport()->height()));
    QCoreApplication::processEvents();

    QCOMPARE(window.lastFullyVisibleMessageId(), ids.at(ids.size() - 2));
    window.hide();
}

QTEST_MAIN(TestChatHistoryWindow)
#include "test_chat_history_window.moc"
