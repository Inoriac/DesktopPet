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

class EmotionAvatarBackend : public AvatarBackend {
public:
    quint64 token = 0;
    QJsonObject resources() const override { return {{"durations",QJsonObject{{"a",1.0},{"b",1.0}}}}; }
    bool start(const QString&,const QString&,bool,quint64 generation,QString*)override{token=generation;return true;}
    void stop(const QString&)override{}
    void setDrawing(bool)override{}
    bool moveTo(const QPoint&,QPoint*)override{return false;}
    void cycle(){emit cycleFinished(token,1);}
};
QJsonObject emotionAvatarConfig() {
    return {{"defaultState","calm"},{"queueLimit",2},{"states",QJsonArray{
        QJsonObject{{"name","calm"},{"loop",true},{"interruption","finish_cycle"},{"clipOptions",QJsonArray{QJsonObject{{"clipName","a"}}}}},
        QJsonObject{{"name","smile"},{"loop",true},{"clipOptions",QJsonArray{QJsonObject{{"clipName","b"}}}}}}},
        {"transactions",QJsonArray{QJsonObject{{"fromState","calm"},{"toState","smile"},{"condition","express"}}}},
        {"semantics",QJsonObject{{"emotion.joy","smile"},{"emotion.anger","smile"},{"emotion.fear","smile"}}}};
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
    AvatarPresentation presentation; EmotionAvatarBackend backend;
    QVERIFY(presentation.configure(emotionAvatarConfig()).isEmpty());
    presentation.setBackend(&backend); presentation.setVisible(true);
    BehaviorManager manager; manager.setPresentation(&presentation);
    QSignalSpy played(&manager,&BehaviorManager::expressionPlayed);
    const auto now=QDateTime::currentDateTimeUtc();
    QCOMPARE(manager.handleExpression(expression(EmotionType::Joy,now),now),ExpressionDisposition::Queued);
    QCOMPARE(manager.pendingCount(),1); QCOMPARE(played.count(),0);
    backend.cycle();
    QCOMPARE(manager.pendingCount(),0); QCOMPARE(played.count(),1);
    QCOMPARE(played[0][1].toString(),"smile");
}
void TestEmotionRuntime::behaviorBoundsQueueAndDropsExpiredRequests() {
    AvatarPresentation presentation; EmotionAvatarBackend backend;
    QVERIFY(presentation.configure(emotionAvatarConfig()).isEmpty());
    presentation.setBackend(&backend); presentation.setVisible(true);
    BehaviorManager manager; manager.setPresentation(&presentation);
    QSignalSpy dropped(&manager,&BehaviorManager::expressionDropped);
    const auto now=QDateTime::currentDateTimeUtc();
    manager.handleExpression(expression(EmotionType::Joy,now,30),now);
    manager.handleExpression(expression(EmotionType::Anger,now,30),now);
    manager.handleExpression(expression(EmotionType::Fear,now,30),now);
    QCOMPARE(manager.pendingCount(),2); QCOMPARE(dropped.count(),1);
    QCOMPARE(dropped[0][1].toString(),"queue_full");
    QTRY_COMPARE(manager.pendingCount(),0); QCOMPARE(dropped.count(),3);
    manager.handleExpression(expression(EmotionType::Joy,now),now);
    presentation.setVisible(false); QCOMPARE(manager.pendingCount(),0);
    presentation.setVisible(true); backend.cycle();
    QCOMPARE(presentation.snapshot()["current_state"].toString(),"calm");
}

void TestEmotionRuntime::behaviorRejectsNonFiniteIntensity() {
    BehaviorManager manager;

    QSignalSpy droppedSpy(&manager, &BehaviorManager::expressionDropped);

    ExpressionRequest request = expression(EmotionType::Joy, testTime());
    request.intensity = std::numeric_limits<double>::quiet_NaN();
    QCOMPARE(static_cast<int>(manager.handleExpression(request, testTime())),
             static_cast<int>(ExpressionDisposition::Dropped));
    QCOMPARE(droppedSpy.count(), 1);
    QCOMPARE(droppedSpy.at(0).at(1).toString(), QStringLiteral("invalid_or_expired"));
}

void TestEmotionRuntime::behaviorDoesNotInventSurpriseAnimation() {
    AvatarPresentation presentation; EmotionAvatarBackend backend;
    presentation.configure(emotionAvatarConfig()); presentation.setBackend(&backend); presentation.setVisible(true);
    BehaviorManager manager; manager.setPresentation(&presentation);

    QSignalSpy droppedSpy(&manager, &BehaviorManager::expressionDropped);

    QCOMPARE(static_cast<int>(manager.handleExpression(
                 expression(EmotionType::Surprise, testTime()), testTime())),
             static_cast<int>(ExpressionDisposition::Dropped));
    QCOMPARE(droppedSpy.count(), 1);
    QCOMPARE(droppedSpy.at(0).at(1).toString(), QStringLiteral("unknown_state"));
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
