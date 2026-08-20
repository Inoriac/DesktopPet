#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

#include "behavior/behavior_manager.h"
#include "controller/pet_controller.h"
#include "emotion/sqlite_emotion_state_repository.h"

#include <cmath>
#include <limits>

namespace {

QDateTime testTime() {
    return QDateTime(QDate(2026, 8, 20), QTime(10, 0), QTimeZone::UTC);
}

ExpressionRequest expression(EmotionType emotion,
                             const QDateTime& now,
                             int lifetimeMs = 5000) {
    ExpressionRequest request;
    request.emotion = emotion;
    request.intensity = 0.9;
    request.requestedAt = now;
    request.expiresAt = now.addMSecs(lifetimeMs);
    request.allowUnsolicited = !isNegativeEmotion(emotion);
    return request;
}

void compareNear(double actual, double expected, double tolerance = 1e-9) {
    QVERIFY(std::abs(actual - expected) <= tolerance);
}

} // namespace

class TestEmotionRuntime : public QObject {
    Q_OBJECT

private slots:
    void sqliteRepositoryRoundTripsState();
    void behaviorQueuesWhileBusyAndPlaysWhenIdle();
    void behaviorBoundsQueueAndDropsExpiredRequests();
    void behaviorRejectsNonFiniteIntensity();
    void behaviorDoesNotInventSurpriseAnimation();
    void controllerProducesBoundedStructuredEvents();
    void controllerTreatsUserBoundariesAsSafe();
};

void TestEmotionRuntime::sqliteRepositoryRoundTripsState() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("nested/emotion.db"));

    {
        SQLiteEmotionStateRepository repository(path);
        QVERIFY(!repository.load().has_value());
        QVERIFY2(repository.lastError().isEmpty(), qPrintable(repository.lastError()));
        QVERIFY(repository.save(PersistedEmotionState{
            1, 0.42, 0.67, testTime(), 3
        }));
        QVERIFY(repository.isOpen());
    }

    {
        SQLiteEmotionStateRepository repository(path);
        const std::optional<PersistedEmotionState> loaded = repository.load();
        QVERIFY2(loaded.has_value(), qPrintable(repository.lastError()));
        QCOMPARE(loaded->schemaVersion, 1);
        compareNear(loaded->moodValence, 0.42);
        compareNear(loaded->moodArousal, 0.67);
        QCOMPARE(loaded->updatedAtUtc, testTime());
        QCOMPARE(loaded->personalityRevision, 3);

        QVERIFY(repository.save(PersistedEmotionState{
            1, -0.15, 0.20, testTime().addSecs(10), 3
        }));
        const std::optional<PersistedEmotionState> updated = repository.load();
        QVERIFY(updated.has_value());
        compareNear(updated->moodValence, -0.15);
        compareNear(updated->moodArousal, 0.20);
    }
}

void TestEmotionRuntime::behaviorQueuesWhileBusyAndPlaysWhenIdle() {
    QString currentState = QStringLiteral("Drag");
    QStringList playedStates;
    BehaviorManager manager(3);
    manager.setCurrentStateProvider([&currentState]() { return currentState; });
    manager.setAnimationPlayer([&playedStates](const QString& state) {
        playedStates.append(state);
        return true;
    });
    QSignalSpy queuedSpy(&manager, &BehaviorManager::expressionQueued);
    QSignalSpy playedSpy(&manager, &BehaviorManager::expressionPlayed);

    const QDateTime now = testTime();
    QCOMPARE(static_cast<int>(manager.handleExpression(expression(EmotionType::Joy, now), now)),
             static_cast<int>(ExpressionDisposition::Queued));
    QCOMPARE(manager.pendingCount(), 1);
    QCOMPARE(queuedSpy.count(), 1);
    QVERIFY(playedStates.isEmpty());

    manager.processPending(now.addSecs(1));
    QVERIFY(playedStates.isEmpty());
    currentState = QStringLiteral("Idle");
    manager.processPending(now.addSecs(2));
    QCOMPARE(playedStates, QStringList{QStringLiteral("Happy")});
    QCOMPARE(manager.pendingCount(), 0);
    QCOMPARE(playedSpy.count(), 1);
}

void TestEmotionRuntime::behaviorBoundsQueueAndDropsExpiredRequests() {
    QString currentState = QStringLiteral("Talk");
    BehaviorManager manager(2);
    manager.setCurrentStateProvider([&currentState]() { return currentState; });
    manager.setAnimationPlayer([](const QString&) { return true; });
    QSignalSpy droppedSpy(&manager, &BehaviorManager::expressionDropped);
    const QDateTime now = testTime();

    manager.handleExpression(expression(EmotionType::Joy, now), now);
    manager.handleExpression(expression(EmotionType::Anger, now), now);
    manager.handleExpression(expression(EmotionType::Fear, now), now);
    QCOMPARE(manager.pendingCount(), 2);
    QCOMPARE(droppedSpy.count(), 1);
    QCOMPARE(droppedSpy.at(0).at(1).toString(), QStringLiteral("queue_full"));

    manager.processPending(now.addSecs(6));
    QCOMPARE(manager.pendingCount(), 0);
    QCOMPARE(droppedSpy.count(), 3);
}

void TestEmotionRuntime::behaviorRejectsNonFiniteIntensity() {
    BehaviorManager manager;
    manager.setCurrentStateProvider([]() { return QStringLiteral("Idle"); });
    manager.setAnimationPlayer([](const QString&) { return true; });
    QSignalSpy droppedSpy(&manager, &BehaviorManager::expressionDropped);

    ExpressionRequest request = expression(EmotionType::Joy, testTime());
    request.intensity = std::numeric_limits<double>::quiet_NaN();
    QCOMPARE(static_cast<int>(manager.handleExpression(request, testTime())),
             static_cast<int>(ExpressionDisposition::Dropped));
    QCOMPARE(droppedSpy.count(), 1);
    QCOMPARE(droppedSpy.at(0).at(1).toString(), QStringLiteral("invalid_or_expired"));
}

void TestEmotionRuntime::behaviorDoesNotInventSurpriseAnimation() {
    BehaviorManager manager;
    manager.setCurrentStateProvider([]() { return QStringLiteral("Idle"); });
    manager.setAnimationPlayer([](const QString&) { return true; });
    QSignalSpy droppedSpy(&manager, &BehaviorManager::expressionDropped);

    QCOMPARE(static_cast<int>(manager.handleExpression(
                 expression(EmotionType::Surprise, testTime()), testTime())),
             static_cast<int>(ExpressionDisposition::Dropped));
    QCOMPARE(droppedSpy.count(), 1);
    QCOMPARE(droppedSpy.at(0).at(1).toString(), QStringLiteral("unsupported_emotion"));
}

void TestEmotionRuntime::controllerProducesBoundedStructuredEvents() {
    EmotionConfig config;
    config.sameSourcePerMinute = 20;
    EmotionEngine engine(config);
    PetController controller(&engine);
    const QDateTime now = testTime();

    QVERIFY(controller.recordTouch(QStringLiteral("Head"), now, QStringLiteral("touch-1")));
    EmotionSnapshot state = engine.snapshot();
    QCOMPARE(static_cast<int>(state.active), static_cast<int>(EmotionType::Joy));
    QVERIFY(state.moodValence > config.baselineValence);
    QVERIFY(state.moodValence <= 1.0);
    QVERIFY(state.moodArousal <= 1.0);

    engine.reset(now.addSecs(1));
    QVERIFY(controller.recordToolOutcome(
        QStringLiteral("web_fetch"), false, now.addSecs(2), QStringLiteral("tool-1")));
    state = engine.snapshot();
    QCOMPARE(static_cast<int>(state.active), static_cast<int>(EmotionType::Neutral));
    QVERIFY(state.moodValence < config.baselineValence);

    engine.reset(now.addSecs(3));
    QVERIFY(controller.recordTaskOutcome(
        QStringLiteral("task-42"), true, now.addSecs(4), QStringLiteral("task-1")));
    QCOMPARE(static_cast<int>(engine.snapshot().active), static_cast<int>(EmotionType::Joy));
}

void TestEmotionRuntime::controllerTreatsUserBoundariesAsSafe() {
    EmotionEngine engine;
    PetController controller(&engine);
    const QDateTime now = testTime();
    const EmotionSnapshot baseline = engine.snapshot();

    QVERIFY(!controller.recordExplicitFeedbackText(
        QStringLiteral("不要这样，关闭这个功能"), now, QStringLiteral("boundary")));
    const EmotionSnapshot afterBoundary = engine.snapshot();
    compareNear(afterBoundary.moodValence, baseline.moodValence);
    compareNear(afterBoundary.moodArousal, baseline.moodArousal);
    QCOMPARE(static_cast<int>(afterBoundary.active), static_cast<int>(EmotionType::Neutral));

    QVERIFY(controller.recordExplicitFeedbackText(
        QStringLiteral("谢谢你，做得很好"), now, QStringLiteral("positive")));
    QCOMPARE(static_cast<int>(engine.snapshot().active), static_cast<int>(EmotionType::Joy));

    engine.reset(now.addSecs(1));
    QVERIFY(controller.recordExplicitFeedbackText(
        QStringLiteral("我不喜欢这个反应"), now.addSecs(2), QStringLiteral("negative")));
    QCOMPARE(static_cast<int>(engine.snapshot().active), static_cast<int>(EmotionType::Neutral));
}

QTEST_MAIN(TestEmotionRuntime)
#include "test_emotion_runtime.moc"
