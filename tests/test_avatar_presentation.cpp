#include "ai/tools/animation_tools.h"
#include "presentation/avatar_presentation.h"
#include <QtTest>

class FakeAvatarBackend : public AvatarBackend {
  public:
    using AvatarBackend::AvatarBackend;
    QJsonObject catalog;
    QString action;
    quint64 token = 0;
    int starts = 0;
    int stops = 0;
    bool drawing = false;
    bool fail = false;
    QPoint position;
    QJsonObject resources() const override {
        return catalog;
    }
    bool start(const QString&, const QString& clip, bool, quint64 generation, QString* error) override {
        if (fail) {
            *error = "backend_start_failed";
            return false;
        }
        action = clip;
        token = generation;
        ++starts;
        return true;
    }
    void stop(const QString&) override {
        ++stops;
        action.clear();
    }
    void setDrawing(bool enabled) override {
        drawing = enabled;
    }
    bool moveTo(const QPoint& p, QPoint* actual) override {
        position = p;
        *actual = p;
        return true;
    }
    void cycle(quint64 generation, quint64 index = 1) {
        emit cycleFinished(generation, index);
    }
};

static QJsonObject definition(QString initial = "rest", QString target = "wave") {
    return {
        {"defaultState", initial},
        {"avatar", QJsonObject{{"description", "A small blue bird"}}},
        {"states", QJsonArray{QJsonObject{{"name", initial},
                                          {"loop", true},
                                          {"interruption", "finish_cycle"},
                                          {"clipOptions", QJsonArray{QJsonObject{{"clipName", "a"}}}}},
                              QJsonObject{{"name", "exit"},
                                          {"loop", false},
                                          {"clipOptions", QJsonArray{QJsonObject{{"clipName", "x"}}}}},
                              QJsonObject{{"name", target},
                                          {"loop", true},
                                          {"clipOptions", QJsonArray{QJsonObject{{"clipName", "b"}}}}}}},
        {"transactions",
         QJsonArray{QJsonObject{
             {"fromState", initial}, {"toState", target}, {"condition", "go"}, {"via", QJsonArray{"exit"}}}}},
        {"semantics", QJsonObject{{"emotion.joy", target}, {"touch.head", target}}}};
}
static QJsonObject resources() {
    return {{"durations", QJsonObject{{"a", 1.0}, {"x", 0.5}, {"b", 1.0}}}};
}

class TestAvatarPresentation : public QObject {
    Q_OBJECT
  private slots:
    void stateNamesAreBackendIndependent();
    void protectionCancellationAndMovement();
    void hiddenResetsAndRejectsLateEvents();
    void actualFailureAndDeadline();
    void interactionFactsSurviveRefusal();
    void invalidAndMissingResources();
    void immediateAndCycleWithoutTransition();
    void cancellationAndExpiryPreserveWholeProtectedPath();
    void multipleQueuedRequestsAndBackendDestruction();
    void scopedPolicyAndFallback();
    void modelToolAliasAndCrossThreadCalls();
    void synchronousObserverCanHideDuringStart();
    void failedBackendCannotHoldProtectedPathForever();
    void expirationObserverCanHideAndClearQueue();
    void malformedActionRulesHaveDiagnostics();
    void disabledInitialStateHasDiagnostic();
    void disabledPathIsNotPlayable();
    void automaticTransitionsRespectSourcePrecedence();
};
void TestAvatarPresentation::stateNamesAreBackendIndependent() {
    for (const auto& names : {QStringList{"rest", "wave"}, QStringList{"cloud", "sparkle"}}) {
        AvatarPresentation p;
        FakeAvatarBackend backend;
        backend.catalog = resources();
        QVERIFY(p.configure(definition(names[0], names[1])).isEmpty());
        p.setBackend(&backend);
        p.setVisible(true);
        PlayAnimationTool tool(&p);
        GetIdleTransitionCandidatesTool candidates(&p);
        QVERIFY(candidates.execute({}).success);
        auto result = tool.execute({{"state", names[1]}});
        QVERIFY(result.success);
        QCOMPARE(result.data["status"].toString(), "queued");
        backend.cycle(backend.token);
        QCOMPARE(p.snapshot()["current_state"].toString(), "exit");
        backend.cycle(backend.token);
        QCOMPARE(p.snapshot()["current_state"].toString(), names[1]);
        QCOMPARE(p.requestResult(result.data["request_id"].toString())["status"].toString(), "completed");
        QCOMPARE(p.capabilities()["avatar"].toObject()["description"].toString(), "A small blue bird");
    }
}
void TestAvatarPresentation::protectionCancellationAndMovement() {
    AvatarPresentation p;
    FakeAvatarBackend b;
    b.catalog = resources();
    p.configure(definition());
    p.setBackend(&b);
    p.setVisible(true);
    const auto r = p.request("wave", AvatarRequestSource::Model);
    const auto id = r["request_id"].toString();
    b.cycle(b.token);
    const auto token = b.token;
    QVERIFY(p.moveTo(QPoint(12, 34))["success"].toBool());
    QCOMPARE(b.position, QPoint(12, 34));
    QCOMPARE(b.token, token);
    QCOMPARE(p.cancel(id)["status"].toString(), "cancel_pending");
    QCOMPARE(p.snapshot()["current_state"].toString(), "exit");
    b.cycle(token);
    QCOMPARE(p.requestResult(id)["status"].toString(), "cancelled");
    const int starts = b.starts;
    b.cycle(token);
    QCOMPARE(b.starts, starts);
}
void TestAvatarPresentation::hiddenResetsAndRejectsLateEvents() {
    AvatarPresentation p;
    FakeAvatarBackend b;
    b.catalog = resources();
    p.configure(definition());
    p.setBackend(&b);
    p.setVisible(true);
    const auto r = p.request("wave");
    b.cycle(b.token);
    const auto old = b.token;
    p.setVisible(false);
    QCOMPARE(p.snapshot()["current_state"].toString(), "rest");
    QVERIFY(!b.drawing);
    QVERIFY(b.action.isEmpty());
    QCOMPARE(p.requestResult(r["request_id"].toString())["status"].toString(), "cancelled");
    QCOMPARE(p.request("wave")["reason"].toString(), "presentation_hidden");
    const int starts = b.starts;
    b.cycle(old);
    QTest::qWait(120);
    QCOMPARE(b.starts, starts);
    p.setVisible(true);
    QCOMPARE(b.starts, starts + 1);
    QCOMPARE(p.snapshot()["current_state"].toString(), "rest");
    b.cycle(old);
    QCOMPARE(b.starts, starts + 1);
}
void TestAvatarPresentation::actualFailureAndDeadline() {
    AvatarPresentation p;
    FakeAvatarBackend b;
    b.catalog = resources();
    p.configure(definition());
    p.setBackend(&b);
    p.setVisible(true);
    const auto r = p.request("wave", AvatarRequestSource::Model, 1);
    QTest::qWait(120);
    QCOMPARE(p.requestResult(r["request_id"].toString())["status"].toString(), "expired");
    b.fail = true;
    const auto r2 = p.request("wave");
    b.cycle(b.token);
    QCOMPARE(p.requestResult(r2["request_id"].toString())["status"].toString(), "failed");
    QVERIFY(p.snapshot()["current_state"].toString() != "wave");
}
void TestAvatarPresentation::interactionFactsSurviveRefusal() {
    AvatarPresentation p;
    FakeAvatarBackend b;
    b.catalog = resources();
    p.configure(definition());
    p.setBackend(&b);
    p.setVisible(true);
    int facts = 0;
    connect(&p, &AvatarPresentation::interactionOccurred, &p,
            [&](const QString&, const QString&) { ++facts; });
    p.setPolicyProvider([] { return QJsonObject{{"emotion", "anger"}, {"intensity", 0.95}}; });
    QCOMPARE(p.interact("touch", "head")["status"].toString(), "declined");
    QCOMPARE(facts, 1);
    p.setVisible(false);
    p.interact("touch", "unknown");
    QCOMPARE(facts, 2);
}
void TestAvatarPresentation::invalidAndMissingResources() {
    AvatarPresentation p;
    auto cfg = definition();
    cfg["defaultState"] = "missing";
    QVERIFY(!p.configure(cfg).isEmpty());
    FakeAvatarBackend b;
    b.catalog = {{"durations", QJsonObject{{"b", 1.0}}}};
    QVERIFY(p.configure(definition()).isEmpty());
    p.setBackend(&b);
    p.setVisible(true);
    QVERIFY(!p.snapshot()["playing"].toBool());
    QCOMPARE(p.snapshot()["current_state"].toString(), "rest");
    auto loop = definition();
    auto states = loop["states"].toArray();
    auto s = states[1].toObject();
    s["loop"] = true;
    states[1] = s;
    loop["states"] = states;
    QVERIFY(!p.configure(loop).isEmpty());
}

void TestAvatarPresentation::immediateAndCycleWithoutTransition() {
    for (const QString rule : {QString{"immediate"}, QString{"finish_cycle"}}) {
        auto cfg = definition();
        auto states = cfg["states"].toArray();
        auto initial = states[0].toObject();
        initial["interruption"] = rule;
        states[0] = initial;
        cfg["states"] = states;
        auto tr = cfg["transactions"].toArray()[0].toObject();
        tr.remove("via");
        cfg["transactions"] = QJsonArray{tr};
        AvatarPresentation p;
        FakeAvatarBackend b;
        b.catalog = resources();
        p.configure(cfg);
        p.setBackend(&b);
        p.setVisible(true);
        const auto r = p.request("wave");
        QCOMPARE(r["status"].toString(), rule == "immediate" ? QString{"completed"} : QString{"queued"});
        if (rule == "finish_cycle") {
            const auto old = b.token;
            b.cycle(old);
            const int starts = b.starts;
            b.cycle(old);
            QCOMPARE(starts, b.starts);
        }
        QCOMPARE(p.snapshot()["current_state"].toString(), "wave");
    }
}
void TestAvatarPresentation::cancellationAndExpiryPreserveWholeProtectedPath() {
    for (bool expired : {false, true}) {
        auto cfg = definition();
        auto states = cfg["states"].toArray();
        auto second = states[1].toObject();
        second["name"] = "exit2";
        states.append(second);
        cfg["states"] = states;
        auto tr = cfg["transactions"].toArray()[0].toObject();
        tr["via"] = QJsonArray{"exit", "exit2"};
        cfg["transactions"] = QJsonArray{tr};
        AvatarPresentation p;
        FakeAvatarBackend b;
        b.catalog = resources();
        p.configure(cfg);
        p.setBackend(&b);
        p.setVisible(true);
        const auto r = p.request("wave", AvatarRequestSource::Model, expired ? 10 : 10000);
        const auto id = r["request_id"].toString();
        b.cycle(b.token);
        const int stops = b.stops;
        if (expired) {
            QTest::qWait(120);
            QCOMPARE(p.requestResult(id)["status"].toString(), "expired");
        } else
            QCOMPARE(p.cancel(id)["status"].toString(), "cancel_pending");
        QCOMPARE(b.stops, stops);
        QCOMPARE(p.snapshot()["current_state"].toString(), "exit");
        b.cycle(b.token);
        QCOMPARE(p.snapshot()["current_state"].toString(), "exit2");
        QCOMPARE(b.stops, stops);
        b.cycle(b.token);
        QCOMPARE(p.snapshot()["current_state"].toString(), "rest");
        QCOMPARE(p.requestResult(id)["status"].toString(),
                 expired ? QString{"expired"} : QString{"cancelled"});
    }
}
void TestAvatarPresentation::multipleQueuedRequestsAndBackendDestruction() {
    AvatarPresentation p;
    auto* b = new FakeAvatarBackend;
    b->catalog = resources();
    auto cfg = definition();
    cfg["queueLimit"] = 2;
    p.configure(cfg);
    p.setBackend(b);
    p.setVisible(true);
    const auto first = p.request("wave", AvatarRequestSource::Model, 10000, {}, "one");
    const auto second = p.request("wave");
    QCOMPARE(p.request("wave")["reason"].toString(), "queue_full");
    QCOMPARE(p.request("wave", AvatarRequestSource::Model, 10000, {}, "one"), first);
    p.setVisible(false);
    QCOMPARE(p.requestResult("one")["status"].toString(), "cancelled");
    QCOMPARE(p.requestResult(second["request_id"].toString())["status"].toString(), "cancelled");
    p.setVisible(true);
    const auto waiting = p.request("wave");
    delete b;
    QCOMPARE(p.requestResult(waiting["request_id"].toString())["status"].toString(), "failed");
    QVERIFY(!p.snapshot()["playing"].toBool());
}
void TestAvatarPresentation::scopedPolicyAndFallback() {
    AvatarPresentation p;
    FakeAvatarBackend b;
    b.catalog = resources();
    auto cfg = definition();
    auto states = cfg["states"].toArray();
    auto missing = states[2].toObject();
    missing["name"] = "unavailable";
    missing["clipOptions"] = QJsonArray{QJsonObject{{"clipName", "missing"}}};
    missing["fallbackState"] = "wave";
    states.append(missing);
    cfg["states"] = states;
    p.configure(cfg);
    p.setBackend(&b);
    p.setVisible(true);
    p.setPolicyProvider([] { return QJsonObject{{"ai_forbidden_states", QJsonArray{"wave"}}}; });
    QCOMPARE(p.request("wave")["reason"].toString(), "state_forbidden");
    QCOMPARE(p.request("wave", AvatarRequestSource::User)["status"].toString(), "queued");
    p.setVisible(false);
    p.setVisible(true);
    p.setPolicyProvider({});
    const auto fallback = p.request("unavailable");
    QVERIFY(fallback["substituted"].toBool());
    QCOMPARE(fallback["target_state"].toString(), "wave");
}
void TestAvatarPresentation::modelToolAliasAndCrossThreadCalls() {
    AvatarPresentation p;
    FakeAvatarBackend b;
    b.catalog = resources();
    p.configure(definition());
    p.setBackend(&b);
    p.setVisible(true);
    RequestIdleTransitionTool tool(&p);
    QVERIFY(tool.validate({{"target_action", "wave"}}));
    const auto r = tool.execute({{"target_action", "wave"}});
    QVERIFY(r.success);
    QCOMPARE(r.data["target_state"].toString(), "wave");
    QJsonObject result;
    QThread* worker = QThread::create([&] {
        result = p.snapshot();
        p.setVisible(false);
    });
    worker->start();
    QTRY_VERIFY(worker->isFinished());
    worker->wait();
    delete worker;
    QCOMPARE(result["current_state"].toString(), "rest");
    QVERIFY(!p.snapshot()["visible"].toBool());
}

void TestAvatarPresentation::synchronousObserverCanHideDuringStart() {
    for (bool stateSignal : {false, true}) {
        AvatarPresentation p;
        FakeAvatarBackend b;
        b.catalog = resources();
        p.configure(definition());
        p.setBackend(&b);
        p.setVisible(true);
        if (stateSignal)
            connect(&p, &AvatarPresentation::stateChanged, &p, [&](const QJsonObject& result) {
                if (result["current_state"] == "exit")
                    p.setVisible(false);
            });
        else
            connect(&p, &AvatarPresentation::requestChanged, &p, [&](const QJsonObject& result) {
                if (result["status"] == "started")
                    p.setVisible(false);
            });
        const auto r = p.request("wave");
        b.cycle(b.token);
        QVERIFY(!p.snapshot()["visible"].toBool());
        QCOMPARE(p.snapshot()["current_state"].toString(), "rest");
        QCOMPARE(p.requestResult(r["request_id"].toString())["status"].toString(), "cancelled");
    }
}

void TestAvatarPresentation::failedBackendCannotHoldProtectedPathForever() {
    AvatarPresentation p;
    FakeAvatarBackend b;
    b.catalog = resources();
    auto durations = b.catalog["durations"].toObject();
    durations["x"] = 0.001;
    b.catalog["durations"] = durations;
    p.configure(definition());
    p.setBackend(&b);
    p.setVisible(true);
    const auto r = p.request("wave", AvatarRequestSource::Model, 15000);
    b.cycle(b.token);
    QTRY_COMPARE_WITH_TIMEOUT(p.requestResult(r["request_id"].toString())["status"].toString(),
                              QString{"failed"}, 6500);
    QCOMPARE(p.requestResult(r["request_id"].toString())["reason"].toString(), "backend_boundary_timeout");
    QCOMPARE(p.snapshot()["current_state"].toString(), "rest");
}
void TestAvatarPresentation::malformedActionRulesHaveDiagnostics() {
    for (const QJsonValue invalid : {QJsonValue(-1), QJsonValue("heavy"), QJsonValue(0)}) {
        AvatarPresentation p;
        auto cfg = definition();
        auto states = cfg["states"].toArray();
        auto state = states[0].toObject();
        auto options = state["clipOptions"].toArray();
        auto option = options[0].toObject();
        option["weight"] = invalid;
        options[0] = option;
        state["clipOptions"] = options;
        states[0] = state;
        cfg["states"] = states;
        QVERIFY(!p.configure(cfg).isEmpty());
    }
    AvatarPresentation p;
    auto cfg = definition();
    auto states = cfg["states"].toArray();
    auto state = states[0].toObject();
    state["interruption"] = "immediate";
    state["clipOptions"] =
        QJsonArray{QJsonObject{{"clipName", "a"}, {"interruption", "complete_transition"}}};
    states[0] = state;
    cfg["states"] = states;
    auto tr = cfg["transactions"].toArray()[0].toObject();
    tr.remove("via");
    cfg["transactions"] = QJsonArray{tr};
    QVERIFY(!p.configure(cfg).isEmpty());
}

void TestAvatarPresentation::expirationObserverCanHideAndClearQueue() {
    AvatarPresentation p;
    FakeAvatarBackend b;
    b.catalog = resources();
    p.configure(definition());
    p.setBackend(&b);
    p.setVisible(true);
    p.request("wave", AvatarRequestSource::Model, 1);
    p.request("wave", AvatarRequestSource::Model, 1);
    p.request("wave", AvatarRequestSource::Model, 10000);
    connect(&p, &AvatarPresentation::requestChanged, &p, [&](const QJsonObject& result) {
        if (result["status"] == "expired")
            p.setVisible(false);
    });
    QTRY_VERIFY(!p.snapshot()["visible"].toBool());
    QCOMPARE(p.snapshot()["queue_size"].toInt(), 0);
}

void TestAvatarPresentation::disabledInitialStateHasDiagnostic() {
    AvatarPresentation p;
    auto cfg = definition();
    auto states = cfg["states"].toArray();
    auto initial = states[0].toObject();
    initial["disabled"] = true;
    states[0] = initial;
    cfg["states"] = states;
    QVERIFY(p.configure(cfg).contains("defaultState: initial state is disabled"));
}

void TestAvatarPresentation::disabledPathIsNotPlayable() {
    for (const bool configured : {true, false}) {
        AvatarPresentation p;
        FakeAvatarBackend b;
        b.catalog = resources();
        auto cfg = definition();
        if (configured) {
            auto states = cfg["states"].toArray();
            auto intermediate = states[1].toObject();
            intermediate["disabled"] = true;
            states[1] = intermediate;
            cfg["states"] = states;
        } else {
            p.setPolicyProvider([] {
                return QJsonObject{{"forbidden_states", QJsonArray{"exit"}}};
            });
        }
        QVERIFY(p.configure(cfg).isEmpty());
        p.setBackend(&b);
        p.setVisible(true);
        for (const auto source : {AvatarRequestSource::Model, AvatarRequestSource::User,
                                  AvatarRequestSource::Emotion, AvatarRequestSource::Automatic}) {
            const auto result = p.request("wave", source);
            QCOMPARE(result["status"].toString(), "rejected");
            QCOMPARE(result["reason"].toString(), "transition_state_forbidden");
        }
        b.cycle(b.token);
        QCOMPARE(b.starts, 1);
        QCOMPARE(p.snapshot()["current_state"].toString(), "rest");
    }
}

void TestAvatarPresentation::automaticTransitionsRespectSourcePrecedence() {
    // The wildcard precedes the concrete edge in the file deliberately.
    for (const int mode : {0, 1, 2}) {
        AvatarPresentation p;
        FakeAvatarBackend b;
        b.catalog = resources();
        auto cfg = definition();
        auto states = cfg["states"].toArray();
        auto initial = states[0].toObject();
        initial["loop"] = false;
        states[0] = initial;
        cfg["states"] = states;
        QJsonArray transitions{QJsonObject{{"fromState", "*"}, {"toState", "wave"}, {"condition", ""}}};
        if (mode == 1)
            transitions.append(QJsonObject{{"fromState", "rest"}, {"toState", "exit"}, {"condition", ""}});
        if (mode == 2)
            transitions.append(QJsonObject{{"fromState", "rest"}, {"toState", "wave"}, {"condition", "explicit"}});
        cfg["transactions"] = transitions;
        QVERIFY(p.configure(cfg).isEmpty());
        p.setBackend(&b);
        p.setVisible(true);
        const auto previous = b.token;
        b.cycle(previous);
        QCOMPARE(p.snapshot()["current_state"].toString(), mode == 0 ? "wave" : mode == 1 ? "exit" : "rest");
        const int starts = b.starts;
        b.cycle(previous);
        QCOMPARE(b.starts, starts);
    }
}
QTEST_GUILESS_MAIN(TestAvatarPresentation)
#include "test_avatar_presentation.moc"
