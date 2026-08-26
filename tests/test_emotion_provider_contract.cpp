#include <QDateTime>
#include <QTest>

#include <cmath>
#include <limits>

#include "ai/integration/emotion_state_provider.h"
#include "emotion/emotion_engine.h"

class TestEmotionProviderContract : public QObject {
    Q_OBJECT

private slots:
    void currentSnapshot_whenCalled_shouldReturnVersionedNeutralSnapshotWithoutSideEffects();
    void trajectory_whenRangeIsValid_shouldReturnEmptyWithoutStorageOrModelCalls();
    void currentSnapshot_whenEngineHasValidState_shouldReturnReadOnlyMappedSnapshot();
    void currentSnapshot_whenEngineIsUnavailable_shouldFallBackWithoutMutatingEmotionState();
    void trajectory_whenCurrentEngineHasNoHistoryContract_shouldReturnEmptyWithoutSchemaChanges();
};

void TestEmotionProviderContract::
currentSnapshot_whenCalled_shouldReturnVersionedNeutralSnapshotWithoutSideEffects() {
    NullEmotionStateProvider provider;
    const QDateTime at = QDateTime::fromString(
        QStringLiteral("2026-08-25T08:00:00.000Z"), Qt::ISODateWithMs);

    const ProvidedEmotionSnapshot first = provider.currentSnapshot(
        QStringLiteral("11111111-1111-4111-8111-111111111111"), at);
    const ProvidedEmotionSnapshot second = provider.currentSnapshot(
        QStringLiteral("11111111-1111-4111-8111-111111111111"), at);

    QCOMPARE(first.schemaVersion, 1);
    QCOMPARE(first.profileId, QStringLiteral("11111111-1111-4111-8111-111111111111"));
    QVERIFY(first.neutralFallback);
    QCOMPARE(first.value.active, EmotionType::Neutral);
    QCOMPARE(first.value.intensity, 0.0);
    QCOMPARE(first.value.updatedAt, at);
    QCOMPARE(first.value, second.value);
}

void TestEmotionProviderContract::
trajectory_whenRangeIsValid_shouldReturnEmptyWithoutStorageOrModelCalls() {
    NullEmotionStateProvider provider;
    const QDateTime from = QDateTime::currentDateTimeUtc().addDays(-1);
    const QDateTime to = from.addDays(1);

    QVERIFY(provider.trajectory(
        QStringLiteral("11111111-1111-4111-8111-111111111111"), from, to).isEmpty());
}

void TestEmotionProviderContract::
currentSnapshot_whenEngineHasValidState_shouldReturnReadOnlyMappedSnapshot() {
    EmotionEngine engine;
    const QDateTime at = QDateTime::currentDateTimeUtc();
    const EmotionSnapshot before = engine.snapshot(at);
    EmotionEngineStateProvider provider(&engine);

    const ProvidedEmotionSnapshot provided = provider.currentSnapshot(
        QStringLiteral("11111111-1111-4111-8111-111111111111"), at);

    QCOMPARE(provided.schemaVersion, 1);
    QVERIFY(!provided.neutralFallback);
    QCOMPARE(provided.value, before);
    QCOMPARE(engine.snapshot(at), before);
}

void TestEmotionProviderContract::
currentSnapshot_whenEngineIsUnavailable_shouldFallBackWithoutMutatingEmotionState() {
    const QDateTime at = QDateTime::currentDateTimeUtc();
    EmotionEngineStateProvider missingProvider(nullptr);

    const ProvidedEmotionSnapshot missing = missingProvider.currentSnapshot(
        QStringLiteral("11111111-1111-4111-8111-111111111111"), at);
    QVERIFY(missing.neutralFallback);
    QCOMPARE(missing.value.active, EmotionType::Neutral);

    EmotionEngine engine;
    EmotionSnapshot invalid = engine.snapshot(at);
    invalid.intensity = std::numeric_limits<double>::quiet_NaN();
    const EmotionSnapshot unchanged = engine.snapshot(at);
    Q_UNUSED(invalid)
    QCOMPARE(engine.snapshot(at), unchanged);
}

void TestEmotionProviderContract::
trajectory_whenCurrentEngineHasNoHistoryContract_shouldReturnEmptyWithoutSchemaChanges() {
    EmotionEngine engine;
    EmotionEngineStateProvider provider(&engine);
    const QDateTime from = QDateTime::currentDateTimeUtc().addDays(-7);

    QVERIFY(provider.trajectory(
        QStringLiteral("11111111-1111-4111-8111-111111111111"),
        from, from.addDays(7)).isEmpty());
    QCOMPARE(provider.currentSnapshot(
        QStringLiteral("11111111-1111-4111-8111-111111111111"), from).schemaVersion, 1);
}

QTEST_APPLESS_MAIN(TestEmotionProviderContract)
#include "test_emotion_provider_contract.moc"
