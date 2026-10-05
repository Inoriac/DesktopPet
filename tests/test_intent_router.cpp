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
    void testReminderOwnsQueryKeywords_data();
    void testReminderOwnsQueryKeywords();
    void testReminderTimeBindsToDuration_data();
    void testReminderTimeBindsToDuration();
    void testAmbiguousReminderUsesModel_data();
    void testAmbiguousReminderUsesModel();
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

void TestIntentRouter::testReminderOwnsQueryKeywords_data() {
    QTest::addColumn<QString>("input");
    QTest::newRow("battery") << QStringLiteral("5分钟后提醒我给电脑充电");
    QTest::newRow("weather") << QStringLiteral("每60分钟提醒我看看天气");
    QTest::newRow("time") << QStringLiteral("5分钟后提醒我确认现在几点");
}

void TestIntentRouter::testReminderOwnsQueryKeywords() {
    QFETCH(QString, input);
    const auto route = IntentRouter().route(input);
    QCOMPARE(route.type, IntentRouteType::DirectToolCall);
    QCOMPARE(route.toolName, QStringLiteral("schedule_create"));
    QCOMPARE(route.toolArguments.value("message").toString(), input);
}

void TestIntentRouter::testReminderTimeBindsToDuration_data() {
    QTest::addColumn<QString>("input");
    QTest::addColumn<QString>("argument");
    QTest::addColumn<qint64>("minutes");
    QTest::newRow("content-before-duration") << QStringLiteral("提醒我复习第2章，30分钟后")
        << QStringLiteral("delay_minutes") << qint64(30);
    QTest::newRow("content-has-hours") << QStringLiteral("每5分钟提醒我休息半小时")
        << QStringLiteral("interval_minutes") << qint64(5);
    QTest::newRow("hours") << QStringLiteral("每隔2小时提醒我喝水")
        << QStringLiteral("interval_minutes") << qint64(120);
    QTest::newRow("delay-hours") << QStringLiteral("2小时后提醒我出门")
        << QStringLiteral("delay_minutes") << qint64(120);
}

void TestIntentRouter::testReminderTimeBindsToDuration() {
    QFETCH(QString, input);
    QFETCH(QString, argument);
    QFETCH(qint64, minutes);
    const auto route = IntentRouter().route(input);
    QCOMPARE(route.type, IntentRouteType::DirectToolCall);
    QCOMPARE(route.toolName, QStringLiteral("schedule_create"));
    QCOMPARE(route.toolArguments.value(argument).toInteger(), minutes);
}

void TestIntentRouter::testAmbiguousReminderUsesModel_data() {
    QTest::addColumn<QString>("input");
    QTest::newRow("chinese") << QStringLiteral("五分钟后提醒我喝水");
    QTest::newRow("decimal") << QStringLiteral("1.5分钟后提醒我喝水");
    QTest::newRow("negative") << QStringLiteral("-5分钟后提醒我喝水");
    QTest::newRow("compound-delay") << QStringLiteral("1小时30分钟后提醒我喝水");
    QTest::newRow("compound-spaces") << QStringLiteral("1小时 30分钟后提醒我喝水");
    QTest::newRow("compound-interval") << QStringLiteral("每1小时30分钟提醒我喝水");
    QTest::newRow("multiple-delays") << QStringLiteral("5分钟后和10分钟后提醒我喝水");
    QTest::newRow("range") << QStringLiteral("5到10分钟后提醒我喝水");
    QTest::newRow("range-spaces") << QStringLiteral("5 到 10分钟后提醒我喝水");
    QTest::newRow("chinese-time-and-content-interval") << QStringLiteral("五分钟后提醒我每5分钟喝水");
    QTest::newRow("chinese-time-and-content-delay") << QStringLiteral("五分钟后提醒我30分钟后出门");
    QTest::newRow("unsupported-outer-interval") << QStringLiteral("每半小时提醒我每5分钟喝水");
    QTest::newRow("zero") << QStringLiteral("0分钟后提醒我喝水");
    QTest::newRow("overflow") << QStringLiteral("999999999999999999999小时后提醒我喝水");
    QTest::newRow("cancel") << QStringLiteral("取消5分钟后提醒我喝水的提醒");
    QTest::newRow("daily") << QStringLiteral("每天8点提醒我看看天气");
}

void TestIntentRouter::testAmbiguousReminderUsesModel() {
    QFETCH(QString, input);
    QCOMPARE(IntentRouter().route(input).type, IntentRouteType::NeedLLM);
}

QTEST_MAIN(TestIntentRouter)
#include "test_intent_router.moc"
