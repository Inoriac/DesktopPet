#include "animation/animation_importer.h"
#include "animation/animation_player.h"
#include "animation/avatar_3d_backend.h"
#include "presentation/avatar_presentation.h"
#include <QTemporaryFile>
#include <QtTest>

class TestAnimationPlayer : public QObject {
    Q_OBJECT
  private slots:
    void samplingAndBlending();
    void invalidTransitionPreservesPlayback();
    void managedBackendReportsRealCyclesAndStopsWhenHidden();
    void managedModeDisablesLegacyAutomaticAndEventTransitions();
    void zeroLengthClipFailsWithoutDivisionByZero();
    void malformedConfigurationDoesNotEscapeImporter();
};
struct AnimationFixture {
    Skeleton skeleton;
    std::unordered_map<std::string, AnimationClip> clips;
    AnimationStateMachineDefinition machine;
    AnimationFixture() {
        skeleton.skinIndex = 0;
        Bone root;
        root.name = "root";
        root.index = 0;
        root.parent = -1;
        skeleton.bones.push_back(root);
        skeleton.nameToIndex["root"] = 0;
        for (const auto& name : {"a", "b"}) {
            AnimationClip clip;
            clip.name = name;
            clip.duration = 1.0;
            AnimationChannel channel;
            channel.boneIndex = 0;
            const float offset = std::string(name) == "a" ? 0 : 20;
            channel.translationKeys = {{0, offset, 0, 0}, {1, offset + 10, 0, 0}};
            clip.channels.push_back(channel);
            clips[name] = clip;
            AnimationState state;
            state.name = name;
            state.loop = true;
            state.clipOptions.push_back({name, 1});
            machine.states.push_back(state);
        }
        machine.defaultState = "a";
        machine.transactions.push_back({"a", "b", "go", 0.2});
        machine.buildLookupTables();
    }
};
void TestAnimationPlayer::samplingAndBlending() {
    AnimationFixture f;
    AnimationPlayer player(f.skeleton, &f.clips, &f.machine);
    player.setMouseTrackingEnabled(false);
    player.update(0.5);
    QCOMPARE(player.currentPose().bonePoses[0].translation.x(), 5.0f);
    QVERIFY(player.changeState("b", 0.2));
    player.update(0.1);
    const auto blended = player.currentPose().bonePoses[0].translation.x();
    QVERIFY(blended > 5 && blended < 21);
    player.update(0.2);
    QCOMPARE(player.currentPose().bonePoses[0].translation.x(), 23.0f);
}
void TestAnimationPlayer::invalidTransitionPreservesPlayback() {
    AnimationFixture f;
    AnimationPlayer player(f.skeleton, &f.clips, &f.machine);
    player.update(0.2);
    QVERIFY(!player.changeState("unknown"));
    QCOMPARE(player.getCurrentClipName(), std::string("a"));
    QCOMPARE(player.getCurrentTimeSeconds(), 0.2);
    f.clips.erase("b");
    QVERIFY(!player.changeState("b"));
    QCOMPARE(player.getCurrentStateName(), std::string("a"));
}
void TestAnimationPlayer::managedBackendReportsRealCyclesAndStopsWhenHidden() {
    AnimationFixture f;
    AnimationPlayer player(f.skeleton, &f.clips, &f.machine);
    bool drawing = false;
    Avatar3DBackend backend(&player, {{"durations", QJsonObject{{"a", 1.0}, {"b", 1.0}}}},
                            [&](bool enabled) { drawing = enabled; }, {});
    AvatarPresentation p;
    QVERIFY(p.configure(
                 {{"defaultState", "nest"},
                  {"states",
                   QJsonArray{QJsonObject{{"name", "nest"},
                                          {"loop", true},
                                          {"interruption", "finish_cycle"},
                                          {"clipOptions", QJsonArray{QJsonObject{{"clipName", "a"}}}}},
                              QJsonObject{{"name", "fly"},
                                          {"loop", true},
                                          {"clipOptions", QJsonArray{QJsonObject{{"clipName", "b"}}}}}}},
                  {"transactions",
                   QJsonArray{QJsonObject{{"fromState", "nest"}, {"toState", "fly"}, {"condition", "go"}}}}})
                .isEmpty());
    p.setBackend(&backend);
    p.setVisible(true);
    QVERIFY(drawing);
    const auto request = p.request("fly");
    QCOMPARE(request["status"].toString(), "queued");
    player.update(0.5);
    QCOMPARE(p.snapshot()["current_state"].toString(), "nest");
    player.update(0.51);
    QCOMPARE(p.snapshot()["current_state"].toString(), "fly");
    QCOMPARE(player.getCurrentStateName(), std::string("fly"));
    p.setVisible(false);
    QVERIFY(!drawing);
    QVERIFY(!player.isPlaying());
    player.update(10);
    QCOMPARE(player.getCurrentTimeSeconds(), 0.0);
    QCOMPARE(player.getCurrentStateName(), std::string("nest"));
    p.setVisible(true);
    QCOMPARE(player.getCurrentTimeSeconds(), 0.0);
    QVERIFY(drawing);
    QVERIFY(player.isPlaying());
}
void TestAnimationPlayer::managedModeDisablesLegacyAutomaticAndEventTransitions() {
    AnimationFixture f;
    f.machine.states[0].loop = false;
    f.machine.transactions[0].condition = "";
    f.machine.buildLookupTables();
    AnimationPlayer player(f.skeleton, &f.clips, &f.machine);
    QString error;
    QVERIFY(player.startManaged("custom", "a", false, 20, &error));
    QSignalSpy cycles(&player, &AnimationPlayer::cycleFinished);
    player.update(1.1);
    player.update(1.1);
    QCOMPARE(cycles.size(), 1);
    QCOMPARE(player.getCurrentStateName(), std::string("custom"));
    player.triggerEvent("go");
    QVERIFY(!player.changeState("b"));
    QCOMPARE(player.getCurrentStateName(), std::string("custom"));
}
void TestAnimationPlayer::zeroLengthClipFailsWithoutDivisionByZero() {
    AnimationFixture f;
    f.clips["b"].duration = 0;
    AnimationPlayer player(f.skeleton, &f.clips, &f.machine);
    QString error;
    QVERIFY(!player.startManaged("bad", "b", true, 1, &error));
    QCOMPARE(player.getCurrentStateName(), std::string("a"));
    f.clips["a"].duration = 0;
    player.update(1);
    QVERIFY(!player.isPlaying());
    QVERIFY(std::isfinite(player.getCurrentTimeSeconds()));
}

void TestAnimationPlayer::malformedConfigurationDoesNotEscapeImporter() {
    QTemporaryFile file;
    QVERIFY(file.open());
    file.write(
        R"({"defaultState":"bad","states":[{"name":"bad","loop":"not-a-boolean"}],"transactions":[]})");
    file.flush();
    AnimationFixture fixture;
    AnimationImporter importer;
    importer.loadStateMachine(file.fileName().toStdString(), fixture.machine);
    QVERIFY(fixture.machine.states.empty());
    QVERIFY(fixture.machine.stateIndexMap.empty());
    AvatarPresentation p;
    const auto invalid =
        QJsonDocument::fromJson(
            R"({"defaultState":"a","states":[{"name":"a"}],"transactions":[{"fromState":"a","toState":"a","via":5}]})")
            .object();
    QVERIFY(!p.configure(invalid).isEmpty());
}
QTEST_GUILESS_MAIN(TestAnimationPlayer)
#include "test_animation_player.moc"
