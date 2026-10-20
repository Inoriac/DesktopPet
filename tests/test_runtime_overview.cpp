#include <QtTest>
#include <QJsonArray>

#include "ai/runtime/runtime_overview.h"

class TestRuntimeOverview : public QObject {
    Q_OBJECT
private slots:
    void runtimeFieldsReflectInputsWithoutPrivateContent();
    void disabledOrUnavailableEmotionHasNoInventedState();
    void runtimeFlagsStayIndependent();
};

void TestRuntimeOverview::runtimeFieldsReflectInputsWithoutPrivateContent() {
    RuntimeOverviewInput input;
    input.profileId = "11111111-1111-4111-8111-111111111111";
    input.petName = "测试角色";
    input.capturedAt = QDateTime::fromString("2026-10-20T16:30:00.123+08:00", Qt::ISODateWithMs);
    input.aiEnabled = true;
    input.storageReady = true;
    input.busy = true;
    input.pendingMessageCount = 2;
    input.autonomy = {{"enabled", true}, {"ready", true}, {"state", "paused"},
        {"error", "MODEL_PROVIDER_ERROR"}, {"isIdle", false}, {"canDeliberate", false},
        {"reason", "user_busy"}, {"paused", true}, {"privateThought", "PRIVATE_THOUGHT"},
        {"task", QJsonObject{{"goal", "PRIVATE_GOAL"}}}};
    input.taskStatus = "paused";
    input.sleeping = true;
    input.daydreamRunning = false;
    input.emotionEnabled = true;
    EmotionSnapshot emotion;
    emotion.active = EmotionType::Joy;
    emotion.moodValence = 0.7;
    emotion.moodArousal = 0.4;
    emotion.intensity = 0.6;
    emotion.confidence = 0.9;
    emotion.sourceEventId = "PRIVATE_EVENT";
    emotion.updatedAt = input.capturedAt.addSecs(-10);
    input.emotion = emotion;
    const QJsonObject counters{{"callCount", 5}, {"successCount", 3}, {"failureCount", 1},
        {"cancelledCount", 1}, {"unknownUsageCount", 1}, {"promptTokens", 100},
        {"completionTokens", 20}, {"totalTokens", 120}, {"reasoningTokens", 5},
        {"cachedTokens", 30}, {"promptCacheHitTokens", 30}, {"promptCacheMissTokens", 10}};
    input.statistics = {{"allTime", counters}, {"today", counters}, {"session", counters},
        {"recentCalls", QJsonArray{}}, {"persistenceWarning", ""}};

    const auto result = runtimeOverviewJson(input);
    QCOMPARE(result["schemaVersion"].toInt(), 1);
    QCOMPARE(result["profileId"].toString(), input.profileId);
    QCOMPARE(result["petName"].toString(), input.petName);
    QCOMPARE(result["capturedAt"].toString(), QString("2026-10-20T08:30:00.123Z"));
    const auto runtime = result["runtime"].toObject();
    QVERIFY(runtime["aiEnabled"].toBool());
    QVERIFY(runtime["storageReady"].toBool());
    QVERIFY(runtime["busy"].toBool());
    QCOMPARE(runtime["pendingMessageCount"].toInt(), 2);
    QCOMPARE(runtime["taskStatus"].toString(), QString("paused"));
    QVERIFY(runtime["sleeping"].toBool());
    QVERIFY(!runtime["daydreamRunning"].toBool());
    auto safeAutonomy = input.autonomy;
    safeAutonomy.remove("privateThought");
    safeAutonomy.remove("task");
    QCOMPARE(runtime["autonomy"].toObject(), safeAutonomy);
    const auto actualEmotion = result["emotion"].toObject();
    QVERIFY(actualEmotion["enabled"].toBool());
    QVERIFY(actualEmotion["available"].toBool());
    QCOMPARE(actualEmotion["active"].toString(), QString("joy"));
    QCOMPARE(actualEmotion["moodValence"].toDouble(), 0.7);
    QCOMPARE(actualEmotion["moodArousal"].toDouble(), 0.4);
    QCOMPARE(actualEmotion["intensity"].toDouble(), 0.6);
    QCOMPARE(actualEmotion["confidence"].toDouble(), 0.9);
    QCOMPARE(actualEmotion["updatedAt"].toString(), QString("2026-10-20T08:29:50.123Z"));
    QCOMPARE(result["statistics"].toObject(), input.statistics);
    QCOMPARE(input.emotion.value(), emotion);
    const QByteArray json = QJsonDocument(result).toJson(QJsonDocument::Compact);
    QVERIFY(!json.contains("PRIVATE_"));
    QVERIFY(!result.contains("messages"));
    QVERIFY(!runtime.contains("task"));
}

void TestRuntimeOverview::disabledOrUnavailableEmotionHasNoInventedState() {
    RuntimeOverviewInput input;
    input.emotion = EmotionSnapshot{};
    QCOMPARE(runtimeOverviewJson(input)["emotion"].toObject(),
             (QJsonObject{{"enabled", false}, {"available", false}}));
    input.emotionEnabled = true;
    input.emotion.reset();
    QCOMPARE(runtimeOverviewJson(input)["emotion"].toObject(),
             (QJsonObject{{"enabled", true}, {"available", false}}));
}

void TestRuntimeOverview::runtimeFlagsStayIndependent() {
    RuntimeOverviewInput input;
    auto runtime = runtimeOverviewJson(input)["runtime"].toObject();
    QVERIFY(!runtime["aiEnabled"].toBool());
    QVERIFY(!runtime["storageReady"].toBool());
    QVERIFY(!runtime["busy"].toBool());
    QVERIFY(!runtime["sleeping"].toBool());
    QVERIFY(!runtime["daydreamRunning"].toBool());
    QCOMPARE(runtime["pendingMessageCount"].toInt(), 0);
    QVERIFY(runtime["taskStatus"].toString().isEmpty());
    input.aiEnabled = true;
    input.pendingMessageCount = 1;
    input.daydreamRunning = true;
    input.autonomy = {{"state", "idle"}, {"isIdle", true}, {"paused", false}};
    runtime = runtimeOverviewJson(input)["runtime"].toObject();
    QVERIFY(runtime["aiEnabled"].toBool());
    QVERIFY(!runtime["busy"].toBool());
    QCOMPARE(runtime["pendingMessageCount"].toInt(), 1);
    QVERIFY(runtime["daydreamRunning"].toBool());
    QCOMPARE(runtime["autonomy"].toObject(), input.autonomy);
}

QTEST_GUILESS_MAIN(TestRuntimeOverview)
#include "test_runtime_overview.moc"
