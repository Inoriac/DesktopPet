//
// 异步 LLM 聊天服务测试
// 验证请求不阻塞事件循环，并验证重试逻辑
//

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <memory>

#include "llm/llm_chat_service.h"
#include "llm/llm_client.h"
#include "configLoader/config_manager.h"
#include "ai/context_builder.h"
#include "statistic_manager.h"

class FakeAsyncLlmClient : public LlmClient {
public:
    struct PlannedResult {
        bool ok = false;
        QString error;
        LlmResponse response;
    };

    QList<PlannedResult> planned;
    int callCount = 0;

    void sendChatCompletionAsync(const LlmConfig&,
                                 const QList<ChatMessage>&,
                                 const QJsonArray&,
                                 LlmCompletionHandler callback) override {
        const int index = callCount;
        callCount += 1;

        PlannedResult result;
        if (index < planned.size()) {
            result = planned[index];
        } else {
            result.ok = false;
            result.error = "No planned result";
        }

        // 使用 0ms singleShot 保证异步回调，不会在当前栈帧阻塞执行。
        QTimer::singleShot(0, [result, callback = std::move(callback)]() mutable {
            callback(result.ok, std::move(result.response), result.error);
        });
    }
};

class TestLlmChatService : public QObject {
    Q_OBJECT

private slots:
    void testRequestAsyncDoesNotBlock();
    void testRetryThenSuccess();
    void testTerminalFailureUpdatesStatistics();
    void testRequestReleasesCapturedState();
    void testDaydreamConfigLoadsAndClamps();
    void testEmotionConfigLoadsFromActiveProfile();
    void testEmotionContextIsReadOnlyAndSafetyBounded();
};

void TestLlmChatService::testRequestAsyncDoesNotBlock() {
    auto fakeClient = std::make_shared<FakeAsyncLlmClient>();
    LlmResponse response;
    response.content = "hello";
    fakeClient->planned.append({true, "", response});

    LlmChatService service(fakeClient);

    LlmConfig cfg;
    cfg.enabled = true;
    cfg.retryCount = 0;

    bool callbackCalled = false;
    QEventLoop loop;

    service.requestAsyncWithConfig(cfg, {}, QJsonArray{},
        [&](bool ok, LlmResponse out, QString err) {
            Q_UNUSED(err)
            QVERIFY(ok);
            QCOMPARE(out.content, QString("hello"));
            callbackCalled = true;
            loop.quit();
        });

    // 如果这里已经被调用，说明是同步阻塞式，不符合目标。
    QVERIFY(!callbackCalled);

    QTimer::singleShot(300, &loop, &QEventLoop::quit);
    loop.exec();

    QVERIFY(callbackCalled);
    QCOMPARE(fakeClient->callCount, 1);
}

void TestLlmChatService::testRetryThenSuccess() {
    auto fakeClient = std::make_shared<FakeAsyncLlmClient>();

    StatisticManager::getInstance().clearStatistics("AI_GLOBAL");

    LlmResponse successResp;
    successResp.content = "retry ok";
    successResp.usage.promptTokens = 11;
    successResp.usage.completionTokens = 22;
    successResp.usage.totalTokens = 33;
    successResp.usage.reasoningTokens = 9;

    fakeClient->planned.append({false, "first failed", {}});
    fakeClient->planned.append({true, "", successResp});

    LlmChatService service(fakeClient);

    LlmConfig cfg;
    cfg.enabled = true;
    cfg.retryCount = 1; // 总尝试次数 = 2

    bool callbackCalled = false;
    QEventLoop loop;

    service.requestAsyncWithConfig(cfg, {}, QJsonArray{},
        [&](bool ok, LlmResponse out, QString err) {
            Q_UNUSED(err)
            QVERIFY(ok);
            QCOMPARE(out.content, QString("retry ok"));
            callbackCalled = true;
            loop.quit();
        });

    QTimer::singleShot(500, &loop, &QEventLoop::quit);
    loop.exec();

    QVERIFY(callbackCalled);
    QCOMPARE(fakeClient->callCount, 2);

    const std::optional<PetStatistics> stats = StatisticManager::getInstance().getPetStatistics("AI_GLOBAL");
    QVERIFY(stats.has_value());
    QCOMPARE(stats->llmCallCount, static_cast<qint64>(1));
    QCOMPARE(stats->llmSuccessCount, static_cast<qint64>(1));
    QCOMPARE(stats->llmFailureCount, static_cast<qint64>(0));
    QCOMPARE(stats->llmPromptTokens, static_cast<qint64>(11));
    QCOMPARE(stats->llmCompletionTokens, static_cast<qint64>(22));
    QCOMPARE(stats->llmTotalTokens, static_cast<qint64>(33));
    QCOMPARE(stats->llmReasoningTokens, static_cast<qint64>(9));
}

void TestLlmChatService::testTerminalFailureUpdatesStatistics() {
    auto fakeClient = std::make_shared<FakeAsyncLlmClient>();
    fakeClient->planned.append({false, "terminal failure", {}});
    StatisticManager::getInstance().clearStatistics("FailurePet");

    LlmChatService service(fakeClient);
    LlmConfig cfg;
    cfg.enabled = true;
    cfg.retryCount = 0;

    bool callbackCalled = false;
    QEventLoop loop;
    service.requestAsyncWithConfig(cfg, {}, {},
        [&](bool ok, LlmResponse, QString error) {
            QVERIFY(!ok);
            QCOMPARE(error, QStringLiteral("terminal failure"));
            callbackCalled = true;
            loop.quit();
        }, QStringLiteral("FailurePet"));
    QTimer::singleShot(300, &loop, &QEventLoop::quit);
    loop.exec();

    QVERIFY(callbackCalled);
    const auto stats = StatisticManager::getInstance()
        .getPetStatistics(QStringLiteral("FailurePet"));
    QVERIFY(stats.has_value());
    QCOMPARE(stats->llmCallCount, static_cast<qint64>(1));
    QCOMPARE(stats->llmSuccessCount, static_cast<qint64>(0));
    QCOMPARE(stats->llmFailureCount, static_cast<qint64>(1));
}

void TestLlmChatService::testRequestReleasesCapturedState() {
    auto fakeClient = std::make_shared<FakeAsyncLlmClient>();
    fakeClient->planned.append({true, {}, {}});
    LlmChatService service(fakeClient);

    LlmConfig cfg;
    cfg.enabled = true;
    cfg.retryCount = 0;

    auto captured = std::make_shared<int>(42);
    std::weak_ptr<int> weakCaptured = captured;
    QEventLoop loop;
    service.requestAsyncWithConfig(cfg, {}, {},
        [captured, &loop](bool, LlmResponse, QString) {
            Q_UNUSED(captured)
            loop.quit();
        });
    captured.reset();
    QTimer::singleShot(300, &loop, &QEventLoop::quit);
    loop.exec();

    QCoreApplication::processEvents();
    QVERIFY(weakCaptured.expired());
}

void TestLlmChatService::testDaydreamConfigLoadsAndClamps() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QJsonObject daydream{
        {"enabled", false},
        {"idleThresholdSec", -1},
        {"dueSoonThresholdMs", -1},
        {"minIntervalMs", 1},
        {"interruptionBackoffMs", -1},
        {"hourlyLimit", 100},
        {"tickIntervalMs", 1},
        {"sessionLimit", 200},
        {"batchLimit", 100},
        {"inboxLimit", 2},
        {"relatedMemoryLimit", 100},
        {"model", "  compact-model  "},
        {"maxTokens", 1},
        {"temperature", 9.0}
    };
    const QJsonObject profile{
        {"enabled", true},
        {"daydream", daydream}
    };
    const QJsonObject root{
        {"aiSettings", QJsonObject{
            {"activeProfile", "selected"},
            {"profiles", QJsonObject{{"selected", profile}}}
        }}
    };

    const QString path = dir.filePath(QStringLiteral("config.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(file.write(QJsonDocument(root).toJson(QJsonDocument::Compact)) > 0);
    file.close();

    ConfigManager& manager = ConfigManager::instance();
    QVERIFY(manager.loadConfig(path));
    const DaydreamConfig& config = manager.getDaydreamConfig();
    QVERIFY(!config.enabled);
    QCOMPARE(config.idleThresholdSec, 30);
    QCOMPARE(config.dueSoonThresholdMs, 0);
    QCOMPARE(config.minIntervalMs, 60000);
    QCOMPARE(config.interruptionBackoffMs, 0);
    QCOMPARE(config.hourlyLimit, 24);
    QCOMPARE(config.tickIntervalMs, 5000);
    QCOMPARE(config.sessionLimit, 128);
    QCOMPARE(config.batchLimit, 32);
    QCOMPARE(config.inboxLimit, 128);
    QCOMPARE(config.relatedMemoryLimit, 32);
}

void TestLlmChatService::testEmotionConfigLoadsFromActiveProfile() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QJsonObject emotion{
        {"enabled", false},
        {"baseline", QJsonObject{{"valence", -0.2}, {"arousal", 0.6}}},
        {"decay", QJsonObject{{"valenceHalfLifeSec", 1800}, {"arousalHalfLifeSec", 900}}},
        {"impulse", QJsonObject{{"sameSourcePerMinute", 5}}},
        {"expression", QJsonObject{{"durationMs", 7000}, {"cooldownMs", 30000}}}
    };
    const QJsonObject root{
        {"aiSettings", QJsonObject{
            {"activeProfile", "selected"},
            {"profiles", QJsonObject{
                {"default", QJsonObject{{"emotion", QJsonObject{{"enabled", true}}}}},
                {"selected", QJsonObject{{"emotion", emotion}}}
            }}
        }}
    };

    const QString path = dir.filePath(QStringLiteral("emotion-config.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(file.write(QJsonDocument(root).toJson(QJsonDocument::Compact)) > 0);
    file.close();

    ConfigManager& manager = ConfigManager::instance();
    QVERIFY(manager.loadConfig(path));
    const EmotionConfig config = manager.getEmotionConfig();
    QVERIFY(!config.enabled);
    QCOMPARE(config.baselineValence, -0.2);
    QCOMPARE(config.baselineArousal, 0.6);
    QCOMPARE(config.valenceHalfLifeSec, 1800.0);
    QCOMPARE(config.arousalHalfLifeSec, 900.0);
    QCOMPARE(config.sameSourcePerMinute, 5);
    QCOMPARE(config.expressionDurationMs, static_cast<qint64>(7000));
    QCOMPARE(config.expressionCooldownMs, static_cast<qint64>(30000));
}

void TestLlmChatService::testEmotionContextIsReadOnlyAndSafetyBounded() {
    EmotionSnapshot emotion;
    emotion.moodValence = -0.25;
    emotion.moodArousal = 0.72;
    emotion.active = EmotionType::Fear;
    emotion.intensity = 0.68;
    emotion.confidence = 0.91;
    emotion.sourceEventId = QStringLiteral("private-event-id");
    emotion.updatedAt = QDateTime::currentDateTimeUtc();

    ContextBuilder builder;
    const QString context = builder.buildRuntimeContext(
        QStringLiteral("TestPet"),
        QStringLiteral("test"),
        QStringLiteral("Idle"),
        QStringLiteral("user_request"),
        {},
        emotion);
    QVERIFY(context.contains(QStringLiteral("mood_valence=-0.25")));
    QVERIFY(context.contains(QStringLiteral("mood_arousal=0.72")));
    QVERIFY(context.contains(QStringLiteral("active_emotion=fear")));
    QVERIFY(context.contains(QStringLiteral("emotion_intensity=0.68")));
    QVERIFY(!context.contains(emotion.sourceEventId));

    const QString systemPrompt = builder.buildSystemPrompt(QStringLiteral("TestPet"));
    QVERIFY(systemPrompt.contains(QStringLiteral("不得降低命令成功率")));
    QVERIFY(systemPrompt.contains(QStringLiteral("不得用悲伤、生气、内疚")));
}

QTEST_MAIN(TestLlmChatService)
#include "test_llm_chat_service.moc"
