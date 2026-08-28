#include <QtTest>

#include <QSignalSpy>
#include <QToolButton>

#include "ui/bubble_playback_controller.h"
#include "ui/liquidglasschatbubble.h"
#include "ui/thinking_status_selector.h"
#include "ai_types.h"

class TestBubblePlaybackController : public QObject {
    Q_OBJECT

private slots:
    void updateDraftPage_whenViewerIsOnLatestPage_shouldUpdateDraftInPlace();
    void updateDraftPage_whenViewerIsReviewingOldPage_shouldNotStealCurrentPage();
    void setHovered_whenAutoTimerIsRunning_shouldResumeFromRemainingDuration();
    void toggleUserPause_whenHovered_shouldPublishResumedStateAfterLeave();
    void nextWhenAutoPlaying_shouldAdvanceWithinBounds();
    void previousWhenUserNavigates_shouldPauseUntilExplicitResume();
    void nextWhenAlreadyOnLastPage_shouldBeNoOp();
    void next_whenStageChanges_shouldUseMatchingPresetPoolWithoutImmediateRepeat();
    void next_whenRequestChanges_shouldResetPerRequestMemory();
    void setDisplayedPage_whenPageCountChanges_shouldKeepTextAndFixedControlsNonOverlapping();
};

void TestBubblePlaybackController::updateDraftPage_whenViewerIsOnLatestPage_shouldUpdateDraftInPlace() {
    BubblePlaybackController controller;
    QSignalSpy changed(&controller, &BubblePlaybackController::pageChanged);
    controller.reset(QStringLiteral("message-id"));

    controller.updateDraftPage(QStringLiteral("正在生"));
    controller.updateDraftPage(QStringLiteral("正在生成"));

    QCOMPARE(controller.currentPageText(), QStringLiteral("正在生成"));
    QCOMPARE(controller.currentIndex(), 0);
    QCOMPARE(controller.pageCount(), 1);
    QVERIFY(controller.currentPageIsDraft());
    QVERIFY(changed.count() >= 2);
}

void TestBubblePlaybackController::updateDraftPage_whenViewerIsReviewingOldPage_shouldNotStealCurrentPage() {
    BubblePlaybackController controller;
    controller.reset(QStringLiteral("message-id"));
    controller.updateDraftPage(QStringLiteral("第一页草稿"));
    controller.appendSealedPages({QStringLiteral("第一页")});
    controller.updateDraftPage(QStringLiteral("第二页草稿"));
    controller.previous();
    QCOMPARE(controller.currentPageText(), QStringLiteral("第一页"));

    controller.updateDraftPage(QStringLiteral("第二页继续增长"));

    QCOMPARE(controller.currentPageText(), QStringLiteral("第一页"));
    QCOMPARE(controller.currentIndex(), 0);
    QCOMPARE(controller.pageCount(), 2);
    QVERIFY(!controller.currentPageIsDraft());
}

void TestBubblePlaybackController::setHovered_whenAutoTimerIsRunning_shouldResumeFromRemainingDuration() {
    BubblePlaybackController controller;
    controller.reset(QStringLiteral("message-id"));
    controller.appendSealedPages(
        {QStringLiteral("第一页"), QStringLiteral("第二页")});
    QVERIFY(controller.autoAdvanceActive());
    const int initialRemaining = controller.remainingAutoAdvanceMs();
    QVERIFY(initialRemaining > 0);
    QTest::qWait(80);

    controller.setHovered(true);
    const int pausedRemaining = controller.remainingAutoAdvanceMs();
    QVERIFY(pausedRemaining > 0);
    QVERIFY(pausedRemaining < initialRemaining);
    QVERIFY(!controller.autoAdvanceActive());
    QVERIFY(controller.isHovered());
    QTest::qWait(80);
    QCOMPARE(controller.remainingAutoAdvanceMs(), pausedRemaining);

    controller.setHovered(false);
    QVERIFY(!controller.isHovered());
    QVERIFY(controller.autoAdvanceActive());
    QVERIFY(controller.remainingAutoAdvanceMs() <= pausedRemaining);
    QVERIFY(controller.remainingAutoAdvanceMs() > pausedRemaining - 100);
}

void TestBubblePlaybackController::toggleUserPause_whenHovered_shouldPublishResumedStateAfterLeave() {
    BubblePlaybackController controller;
    controller.reset(QStringLiteral("message-id"));
    controller.appendSealedPages(
        {QStringLiteral("一"), QStringLiteral("二"), QStringLiteral("三")});
    controller.next();
    QVERIFY(controller.isUserPaused());
    QSignalSpy stateChanged(
        &controller, &BubblePlaybackController::playbackStateChanged);

    controller.setHovered(true);
    controller.toggleUserPause();
    QVERIFY(!controller.isUserPaused());
    QVERIFY(controller.isHovered());
    QCOMPARE(stateChanged.count(), 0);

    controller.setHovered(false);
    QCOMPARE(stateChanged.count(), 1);
    QCOMPARE(stateChanged.first().first().toBool(), false);
    QVERIFY(controller.autoAdvanceActive());
}

void TestBubblePlaybackController::nextWhenAutoPlaying_shouldAdvanceWithinBounds() {
    BubblePlaybackController controller;
    controller.reset(QStringLiteral("message-id"));
    controller.appendSealedPages(
        {QStringLiteral("一"), QStringLiteral("二"), QStringLiteral("三")});

    controller.next();

    QCOMPARE(controller.currentIndex(), 1);
    QCOMPARE(controller.currentPageText(), QStringLiteral("二"));
    QVERIFY(controller.isUserPaused());
}

void TestBubblePlaybackController::previousWhenUserNavigates_shouldPauseUntilExplicitResume() {
    BubblePlaybackController controller;
    controller.reset(QStringLiteral("message-id"));
    controller.appendSealedPages(
        {QStringLiteral("一"), QStringLiteral("二"), QStringLiteral("三")});
    controller.next();
    controller.toggleUserPause();
    QVERIFY(controller.autoAdvanceActive());

    controller.previous();

    QCOMPARE(controller.currentIndex(), 0);
    QVERIFY(controller.isUserPaused());
    QVERIFY(!controller.autoAdvanceActive());
    controller.toggleUserPause();
    QVERIFY(!controller.isUserPaused());
    QVERIFY(controller.autoAdvanceActive());
}

void TestBubblePlaybackController::nextWhenAlreadyOnLastPage_shouldBeNoOp() {
    BubblePlaybackController controller;
    QSignalSpy changed(&controller, &BubblePlaybackController::pageChanged);
    controller.reset(QStringLiteral("message-id"));
    controller.appendSealedPages({QStringLiteral("唯一一页")});
    changed.clear();

    controller.next();

    QCOMPARE(controller.currentIndex(), 0);
    QVERIFY(!controller.isUserPaused());
    QCOMPARE(changed.count(), 0);
}

void TestBubblePlaybackController::next_whenStageChanges_shouldUseMatchingPresetPoolWithoutImmediateRepeat() {
    ThinkingStatusSelector selector;

    const QString waitingFirst = selector.next(
        ChatActivityStage::WaitingForModel, QStringLiteral("request"));
    const QString waitingSecond = selector.next(
        ChatActivityStage::WaitingForModel, QStringLiteral("request"));
    const QString toolFirst = selector.next(
        ChatActivityStage::RunningTool, QStringLiteral("request"));
    const QString toolSecond = selector.next(
        ChatActivityStage::RunningTool, QStringLiteral("request"));
    const QString finalizing = selector.next(
        ChatActivityStage::Finalizing, QStringLiteral("request"));

    QVERIFY(!waitingFirst.isEmpty());
    QVERIFY(waitingFirst != waitingSecond);
    QVERIFY(!toolFirst.isEmpty());
    QVERIFY(toolFirst != toolSecond);
    QVERIFY(waitingFirst != toolFirst);
    QVERIFY(toolFirst != finalizing);
}

void TestBubblePlaybackController::next_whenRequestChanges_shouldResetPerRequestMemory() {
    ThinkingStatusSelector selector;
    const QString first = selector.next(
        ChatActivityStage::WaitingForModel, QStringLiteral("request-a"));
    const QString second = selector.next(
        ChatActivityStage::WaitingForModel, QStringLiteral("request-a"));

    const QString resetFirst = selector.next(
        ChatActivityStage::WaitingForModel, QStringLiteral("request-b"));

    QVERIFY(first != second);
    QCOMPARE(resetFirst, first);
}

void TestBubblePlaybackController::setDisplayedPage_whenPageCountChanges_shouldKeepTextAndFixedControlsNonOverlapping() {
    LiquidGlassChatBubble bubble;
    bubble.showStreamingMessage(QStringLiteral("message-id"));
    bubble.setDisplayedPage(QStringLiteral("一段用于测量布局的正文"), 0, 1, true);
    bubble.resize(bubble.sizeHint());

    const QRect firstTextRect = bubble.displayedTextRect();
    const QRect firstControlsRect = bubble.playbackControlsRect();
    QVERIFY(!firstTextRect.intersects(firstControlsRect));
    QVERIFY(firstControlsRect.height() >= 32);

    bubble.setDisplayedPage(QStringLiteral("一段用于测量布局的正文"), 8, 12, false);
    bubble.resize(bubble.sizeHint());
    QVERIFY(!bubble.displayedTextRect().intersects(
        bubble.playbackControlsRect()));
    const QStringList names = {
        QStringLiteral("previousPageButton"),
        QStringLiteral("nextPageButton"),
        QStringLiteral("playbackToggleButton"),
        QStringLiteral("openConversationButton")};
    for (const QString& name : names) {
        QToolButton* button = bubble.findChild<QToolButton*>(name);
        QVERIFY2(button, qPrintable(name));
        QCOMPARE(button->size(), QSize(32, 32));
        QVERIFY(!button->toolTip().isEmpty());
        QVERIFY(!button->accessibleName().isEmpty());
        QVERIFY(bubble.playbackControlsRect().contains(button->geometry()));
    }

    ScreenChatConfig largeTextConfig;
    largeTextConfig.bubbleFontSize = 36;
    bubble.applyScreenChatConfig(largeTextConfig);
    bubble.setDisplayedPage(QString(180, QChar(u'长')), 0, 1, false);
    bubble.resize(bubble.sizeHint());
    QVERIFY(bubble.height() <= 400);
    QVERIFY(bubble.displayedTextRect().contains(
        bubble.displayedTextBoundingRect()));
}

QTEST_MAIN(TestBubblePlaybackController)
#include "test_bubble_playback_controller.moc"
