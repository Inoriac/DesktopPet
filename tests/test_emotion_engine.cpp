#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
#include <QTimeZone>

#include <cmath>
#include <limits>
#include <optional>

#include "emotion/emotion_config_json.h"
#include "emotion/emotion_engine.h"

namespace {

constexpr double kTolerance = 1e-9;

QDateTime testTime() {
    return QDateTime(QDate(2026, 8, 19), QTime(12, 0), QTimeZone::UTC);
}

AffectEvent baseEvent(const QString& id) {
    AffectEvent event;
    event.id = id;
    event.kind = AffectEventKind::NovelEvent;
    event.source = AffectSource::System;
    event.sourceId = id;
    event.relevance = 1.0;
    event.confidence = 1.0;
    event.certainty = 1.0;
    event.controllability = 0.5;
    return event;
}

AffectEvent joyEvent(const QString& id, double strength = 1.0) {
    AffectEvent event = baseEvent(id);
    event.kind = AffectEventKind::TaskSucceeded;
    event.goalCongruence = strength;
    event.outcome = AffectOutcome::Success;
    event.agency = AffectAgency::Self;
    return event;
}

void compareNear(double actual, double expected, double tolerance = kTolerance) {
    QVERIFY2(std::abs(actual - expected) <= tolerance,
             qPrintable(QStringLiteral("actual=%1 expected=%2 tolerance=%3")
                            .arg(actual, 0, 'g', 16)
                            .arg(expected, 0, 'g', 16)
                            .arg(tolerance, 0, 'g', 16)));
}

class FakeEmotionRepository final : public IEmotionStateRepository {
public:
    std::optional<PersistedEmotionState> stored;
    bool saveResult = true;
    int loadCount = 0;
    int saveCount = 0;

    std::optional<PersistedEmotionState> load() override {
        ++loadCount;
        return stored;
    }

    bool save(const PersistedEmotionState& state) override {
        ++saveCount;
        if (saveResult) {
            stored = state;
        }
        return saveResult;
    }
};

} // namespace

class TestEmotionEngine : public QObject {
    Q_OBJECT

private slots:
    void configParsingUsesStrictBounds();
    void configSanitizesNonFiniteValues();
    void halfLifeDecayIsCorrect();
    void decayIsIndependentOfTickGranularity();
    void mapsAppraisalsToExpectedEmotions_data();
    void mapsAppraisalsToExpectedEmotions();
    void rejectsInvalidDuplicateAndRateLimitedEvents();
    void appliesHysteresisAndExpressionCooldown();
    void disableResetAndProtectedEventsAreSafe();
    void restoresAndDecaysPersistedMood();
    void persistenceFailuresAreObservable();
};

void TestEmotionEngine::configParsingUsesStrictBounds() {
    const QJsonObject object{
        {"enabled", "yes"},
        {"baseline", QJsonObject{{"valence", 9.0}, {"arousal", "high"}}},
        {"decay", QJsonObject{
            {"valenceHalfLifeSec", -10.0},
            {"arousalHalfLifeSec", 999999999.0},
            {"maxOfflineDecaySec", -4.0}
        }},
        {"impulse", QJsonObject{
            {"maxValence", 2.0},
            {"maxArousal", -1.0},
            {"negativeMultiplier", 3.0},
            {"sameSourcePerMinute", 2.5}
        }},
        {"expression", QJsonObject{
            {"positiveThreshold", 0.8},
            {"negativeThreshold", 0.2},
            {"switchMargin", -1.0},
            {"minIntensity", 8.0},
            {"durationMs", -1},
            {"cooldownMs", -1},
            {"queueLimit", 100}
        }},
        {"llmAppraisal", QJsonObject{{"enabled", true}, {"minConfidence", 4.0}}}
    };

    const EmotionConfig config = parseEmotionConfig(object);
    QVERIFY(config.enabled);
    QCOMPARE(config.baselineValence, 1.0);
    QCOMPARE(config.baselineArousal, 0.35);
    QCOMPARE(config.valenceHalfLifeSec, 1.0);
    QCOMPARE(config.arousalHalfLifeSec, 30.0 * 24.0 * 3600.0);
    QCOMPARE(config.maxOfflineDecaySec, 0.0);
    QCOMPARE(config.maxValenceImpulse, 1.0);
    QCOMPARE(config.maxArousalImpulse, 0.0);
    QCOMPARE(config.negativeMultiplier, 1.0);
    QCOMPARE(config.sameSourcePerMinute, 3);
    QCOMPARE(config.positiveThreshold, 0.8);
    QCOMPARE(config.negativeThreshold, 0.8);
    QCOMPARE(config.switchMargin, 0.0);
    QCOMPARE(config.minExpressionIntensity, 1.0);
    QCOMPARE(config.expressionDurationMs, static_cast<qint64>(100));
    QCOMPARE(config.expressionCooldownMs, static_cast<qint64>(0));
    QCOMPARE(config.expressionQueueLimit, 32);
    QVERIFY(config.llmAppraisalEnabled);
    QCOMPARE(config.llmMinConfidence, 1.0);

    const EmotionConfig hugeIntegers = parseEmotionConfig(QJsonObject{
        {"impulse", QJsonObject{{"sameSourcePerMinute", 1e100}}},
        {"expression", QJsonObject{{"durationMs", 1e100}, {"queueLimit", 1e100}}}
    });
    QCOMPARE(hugeIntegers.sameSourcePerMinute, EmotionConfig{}.sameSourcePerMinute);
    QCOMPARE(hugeIntegers.expressionDurationMs, EmotionConfig{}.expressionDurationMs);
    QCOMPARE(hugeIntegers.expressionQueueLimit, EmotionConfig{}.expressionQueueLimit);
}

void TestEmotionEngine::configSanitizesNonFiniteValues() {
    EmotionConfig input;
    input.baselineValence = std::numeric_limits<double>::quiet_NaN();
    input.baselineArousal = std::numeric_limits<double>::infinity();
    input.valenceHalfLifeSec = -std::numeric_limits<double>::infinity();
    input.positiveThreshold = std::numeric_limits<double>::quiet_NaN();

    const EmotionConfig config = sanitizeEmotionConfig(input);
    QCOMPARE(config.baselineValence, EmotionConfig{}.baselineValence);
    QCOMPARE(config.baselineArousal, EmotionConfig{}.baselineArousal);
    QCOMPARE(config.valenceHalfLifeSec, EmotionConfig{}.valenceHalfLifeSec);
    QCOMPARE(config.positiveThreshold, EmotionConfig{}.positiveThreshold);
}

void TestEmotionEngine::halfLifeDecayIsCorrect() {
    EmotionEngine engine;
    const QDateTime start = testTime();
    QVERIFY(engine.submitEvent(joyEvent(QStringLiteral("half-life")), start));

    const EmotionSnapshot immediate = engine.snapshot();
    compareNear(immediate.moodValence, 0.28);
    compareNear(immediate.moodArousal, 0.60);

    const EmotionSnapshot afterHour = engine.snapshot(start.addSecs(3600));
    compareNear(afterHour.moodValence, 0.19);
    compareNear(afterHour.moodArousal, 0.38125);
    QCOMPARE(static_cast<int>(afterHour.active), static_cast<int>(EmotionType::Neutral));
}

void TestEmotionEngine::decayIsIndependentOfTickGranularity() {
    EmotionConfig config;
    config.valenceHalfLifeSec = 60.0;
    config.arousalHalfLifeSec = 60.0;
    EmotionEngine oneStep(config);
    EmotionEngine sixSteps(config);
    const QDateTime start = testTime();

    QVERIFY(oneStep.submitEvent(joyEvent(QStringLiteral("one")), start));
    QVERIFY(sixSteps.submitEvent(joyEvent(QStringLiteral("six")), start));
    oneStep.advanceTo(start.addSecs(60));
    for (int step = 1; step <= 6; ++step) {
        sixSteps.advanceTo(start.addSecs(step * 10));
    }

    const EmotionSnapshot left = oneStep.snapshot();
    const EmotionSnapshot right = sixSteps.snapshot();
    compareNear(left.moodValence, right.moodValence);
    compareNear(left.moodArousal, right.moodArousal);
}

void TestEmotionEngine::mapsAppraisalsToExpectedEmotions_data() {
    QTest::addColumn<int>("caseId");
    QTest::addColumn<int>("expectedEmotion");

    QTest::newRow("success-to-joy") << 0 << static_cast<int>(EmotionType::Joy);
    QTest::newRow("loss-to-sadness") << 1 << static_cast<int>(EmotionType::Sadness);
    QTest::newRow("obstruction-to-anger") << 2 << static_cast<int>(EmotionType::Anger);
    QTest::newRow("threat-to-fear") << 3 << static_cast<int>(EmotionType::Fear);
    QTest::newRow("novelty-to-surprise") << 4 << static_cast<int>(EmotionType::Surprise);
}

void TestEmotionEngine::mapsAppraisalsToExpectedEmotions() {
    QFETCH(int, caseId);
    QFETCH(int, expectedEmotion);

    AffectEvent event = baseEvent(QStringLiteral("mapping-%1").arg(caseId));
    switch (caseId) {
    case 0:
        event = joyEvent(event.id);
        break;
    case 1:
        event.kind = AffectEventKind::ConfirmedLoss;
        event.goalCongruence = -1.0;
        event.controllability = 0.0;
        event.outcome = AffectOutcome::Loss;
        break;
    case 2:
        event.kind = AffectEventKind::ExternalObstruction;
        event.goalCongruence = -1.0;
        event.controllability = 1.0;
        event.agency = AffectAgency::Environment;
        break;
    case 3:
        event.kind = AffectEventKind::UncertainThreat;
        event.goalCongruence = -1.0;
        event.certainty = 0.0;
        event.controllability = 0.0;
        break;
    case 4:
        event.kind = AffectEventKind::NovelEvent;
        event.novelty = 1.0;
        break;
    default:
        QFAIL("Unexpected mapping case");
    }

    EmotionEngine engine;
    QVERIFY(engine.submitEvent(event, testTime()));
    QCOMPARE(static_cast<int>(engine.snapshot().active), expectedEmotion);
}

void TestEmotionEngine::rejectsInvalidDuplicateAndRateLimitedEvents() {
    EmotionConfig config;
    config.sameSourcePerMinute = 2;
    EmotionEngine engine(config);
    const QDateTime start = testTime();

    AffectEvent invalid = joyEvent(QStringLiteral("invalid"));
    invalid.relevance = std::numeric_limits<double>::quiet_NaN();
    QVERIFY(!engine.submitEvent(invalid, start));

    AffectEvent unspecified = joyEvent(QStringLiteral("unspecified"));
    unspecified.kind = AffectEventKind::Unspecified;
    QVERIFY(!engine.submitEvent(unspecified, start));

    AffectEvent unknownSource = joyEvent(QStringLiteral("unknown-source"));
    unknownSource.source = AffectSource::Unknown;
    QVERIFY(!engine.submitEvent(unknownSource, start));

    AffectEvent future = joyEvent(QStringLiteral("future"));
    future.occurredAt = start.addSecs(301);
    QVERIFY(!engine.submitEvent(future, start));

    AffectEvent first = joyEvent(QStringLiteral("first"));
    first.sourceId = QStringLiteral("same-source");
    QVERIFY(engine.submitEvent(first, start));
    QVERIFY(!engine.submitEvent(first, start.addMSecs(1)));

    AffectEvent second = joyEvent(QStringLiteral("second"));
    second.sourceId = QStringLiteral("same-source");
    QVERIFY(engine.submitEvent(second, start.addSecs(1)));

    AffectEvent third = joyEvent(QStringLiteral("third"));
    third.sourceId = QStringLiteral("same-source");
    QVERIFY(!engine.submitEvent(third, start.addSecs(2)));

    AffectEvent afterWindow = joyEvent(QStringLiteral("after-window"));
    afterWindow.sourceId = QStringLiteral("same-source");
    QVERIFY(engine.submitEvent(afterWindow, start.addMSecs(60001)));
}

void TestEmotionEngine::appliesHysteresisAndExpressionCooldown() {
    EmotionConfig config;
    config.switchMargin = 0.20;
    config.expressionDurationMs = 120000;
    config.expressionCooldownMs = 60000;
    config.sameSourcePerMinute = 10;
    EmotionEngine engine(config);
    QSignalSpy expressionSpy(&engine, &EmotionEngine::expressionRequested);
    const QDateTime start = testTime();

    QVERIFY(engine.submitEvent(joyEvent(QStringLiteral("joy-1"), 0.8), start));
    QCOMPARE(expressionSpy.count(), 1);

    AffectEvent weakLoss = baseEvent(QStringLiteral("loss-weak"));
    weakLoss.kind = AffectEventKind::ConfirmedLoss;
    weakLoss.goalCongruence = -0.9;
    weakLoss.controllability = 0.0;
    weakLoss.outcome = AffectOutcome::Loss;
    QVERIFY(engine.submitEvent(weakLoss, start.addSecs(1)));
    QCOMPARE(static_cast<int>(engine.snapshot().active), static_cast<int>(EmotionType::Joy));

    AffectEvent strongLoss = weakLoss;
    strongLoss.id = QStringLiteral("loss-strong");
    strongLoss.sourceId = strongLoss.id;
    strongLoss.goalCongruence = -1.0;
    QVERIFY(engine.submitEvent(strongLoss, start.addSecs(2)));
    QCOMPARE(static_cast<int>(engine.snapshot().active), static_cast<int>(EmotionType::Sadness));
    QCOMPARE(expressionSpy.count(), 2);
    const ExpressionRequest negativeRequest = qvariant_cast<ExpressionRequest>(expressionSpy.at(1).at(0));
    QVERIFY(!negativeRequest.allowUnsolicited);

    AffectEvent repeatedLoss = strongLoss;
    repeatedLoss.id = QStringLiteral("loss-repeat");
    repeatedLoss.sourceId = repeatedLoss.id;
    QVERIFY(engine.submitEvent(repeatedLoss, start.addSecs(10)));
    QCOMPARE(expressionSpy.count(), 2);

    repeatedLoss.id = QStringLiteral("loss-after-cooldown");
    repeatedLoss.sourceId = repeatedLoss.id;
    QVERIFY(engine.submitEvent(repeatedLoss, start.addSecs(63)));
    QCOMPARE(expressionSpy.count(), 3);
}

void TestEmotionEngine::disableResetAndProtectedEventsAreSafe() {
    EmotionEngine engine;
    const QDateTime start = testTime();
    const QList<AffectEventKind> protectedKinds{
        AffectEventKind::MemoryRecalled,
        AffectEventKind::UserIdle,
        AffectEventKind::ApplicationExit,
        AffectEventKind::UserRefusal,
        AffectEventKind::UserCorrection,
        AffectEventKind::PrivacyChanged,
        AffectEventKind::MemoryDeleted,
        AffectEventKind::EmotionDisabled
    };

    int index = 0;
    for (AffectEventKind kind : protectedKinds) {
        AffectEvent event = baseEvent(QStringLiteral("protected-%1").arg(index++));
        event.kind = kind;
        event.goalCongruence = -1.0;
        event.controllability = 0.0;
        event.outcome = AffectOutcome::Loss;
        QVERIFY(!engine.submitEvent(event, start));
    }
    compareNear(engine.snapshot().moodValence, EmotionConfig{}.baselineValence);

    QVERIFY(engine.submitEvent(joyEvent(QStringLiteral("before-disable")), start));
    engine.setEnabled(false, start.addSecs(1));
    QVERIFY(!engine.isEnabled());
    compareNear(engine.snapshot().moodValence, EmotionConfig{}.baselineValence);
    QCOMPARE(static_cast<int>(engine.snapshot().active), static_cast<int>(EmotionType::Neutral));
    QVERIFY(!engine.submitEvent(joyEvent(QStringLiteral("while-disabled")), start.addSecs(2)));

    engine.setEnabled(true, start.addSecs(3));
    QVERIFY(engine.isEnabled());
    QVERIFY(engine.submitEvent(joyEvent(QStringLiteral("after-enable")), start.addSecs(4)));
    engine.reset(start.addSecs(5));
    compareNear(engine.snapshot().moodValence, EmotionConfig{}.baselineValence);
    compareNear(engine.snapshot().moodArousal, EmotionConfig{}.baselineArousal);
    QCOMPARE(static_cast<int>(engine.snapshot().active), static_cast<int>(EmotionType::Neutral));
}

void TestEmotionEngine::restoresAndDecaysPersistedMood() {
    EmotionConfig config;
    config.valenceHalfLifeSec = 100.0;
    config.arousalHalfLifeSec = 100.0;
    config.maxOfflineDecaySec = 1000.0;
    FakeEmotionRepository repository;
    repository.stored = PersistedEmotionState{
        1,
        0.9,
        0.95,
        testTime(),
        config.personalityRevision
    };

    EmotionEngine engine(config, &repository);
    QVERIFY(engine.restore(testTime().addSecs(100)));
    QCOMPARE(repository.loadCount, 1);
    const EmotionSnapshot state = engine.snapshot();
    compareNear(state.moodValence, 0.5);
    compareNear(state.moodArousal, 0.65);
    QCOMPARE(static_cast<int>(state.active), static_cast<int>(EmotionType::Neutral));

    repository.stored->updatedAtUtc = testTime().addSecs(-2000);
    QVERIFY(engine.restore(testTime()));
    compareNear(engine.snapshot().moodValence, config.baselineValence);
    compareNear(engine.snapshot().moodArousal, config.baselineArousal);

    repository.stored->updatedAtUtc = testTime().addSecs(301);
    repository.stored->moodValence = 0.9;
    QVERIFY(!engine.restore(testTime()));
    compareNear(engine.snapshot().moodValence, config.baselineValence);
}

void TestEmotionEngine::persistenceFailuresAreObservable() {
    FakeEmotionRepository repository;
    repository.saveResult = false;
    EmotionEngine engine({}, &repository);
    QSignalSpy failureSpy(&engine, &EmotionEngine::persistenceFailed);

    QVERIFY(engine.submitEvent(joyEvent(QStringLiteral("save-failure")), testTime()));
    QCOMPARE(repository.saveCount, 1);
    QCOMPARE(failureSpy.count(), 1);
    QCOMPARE(failureSpy.at(0).at(0).toString(), QStringLiteral("save"));
}

QTEST_MAIN(TestEmotionEngine)
#include "test_emotion_engine.moc"
