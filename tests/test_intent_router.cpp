//
// IntentRouter tests
//

#include <QTest>

#include "router/intent_router.h"

class TestIntentRouter : public QObject {
    Q_OBJECT

private slots:
    void testWeatherQueryUsesExplicitCity();
    void testWeatherQueryAsksForLocation();
    void testGreetingOnlyGetsQuickReply();
    void testGreetingWithContentIsNotSwallowed();
    void testDailyBriefingUsesExplicitCity();
};

void TestIntentRouter::testWeatherQueryUsesExplicitCity() {
    IntentRouter router;
    const IntentRoute route = router.route("查看长沙天气", "user_request");

    QCOMPARE(route.type, IntentRouteType::DirectToolCall);
    QCOMPARE(route.toolName, QString("weather_query"));
    QCOMPARE(route.toolArguments.value("location").toString(), QString("长沙"));
}

void TestIntentRouter::testWeatherQueryAsksForLocation() {
    IntentRouter router;
    const IntentRoute route = router.route("查看天气", "user_request");

    QCOMPARE(route.type, IntentRouteType::NeedClarification);
    QCOMPARE(route.reason, QString("weather_missing_location"));
    QVERIFY(route.reply.contains(QStringLiteral("城市")) || route.reply.contains(QStringLiteral("地点")));
}

void TestIntentRouter::testGreetingOnlyGetsQuickReply() {
    IntentRouter router;
    const IntentRoute route = router.route("你好呀！", "user_request");

    QCOMPARE(route.type, IntentRouteType::DirectReply);
    QCOMPARE(route.reason, QString("simple_greeting"));
    QCOMPARE(route.reply, QStringLiteral("在哦。"));
}

void TestIntentRouter::testGreetingWithContentIsNotSwallowed() {
    IntentRouter router;

    const IntentRoute chatRoute = router.route("你好，帮我讲个笑话", "user_request");
    QVERIFY(chatRoute.type != IntentRouteType::DirectReply || chatRoute.reason != QString("simple_greeting"));

    const IntentRoute weatherRoute = router.route("你好，帮我看看今天的天气", "user_request");
    QCOMPARE(weatherRoute.type, IntentRouteType::NeedClarification);
    QCOMPARE(weatherRoute.reason, QString("weather_missing_location"));
}

void TestIntentRouter::testDailyBriefingUsesExplicitCity() {
    IntentRouter router;
    const IntentRoute route = router.route("长沙今日简报", "user_request");

    QCOMPARE(route.type, IntentRouteType::DirectToolCall);
    QCOMPARE(route.toolName, QString("daily_briefing"));
    QCOMPARE(route.toolArguments.value("location").toString(), QString("长沙"));
}

QTEST_MAIN(TestIntentRouter)
#include "test_intent_router.moc"
