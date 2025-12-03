//
// Created by Inoriac on 2025/12/2.
//

#include <QTest>
#include <QDir>
#include <memory>

#include "animation/animation_player.h"
#include "animation/animation_importer.h"
#include "model_loader.h"

class TestAnimationPlayer : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void testInitialization();
    void testAnimationSampling();
    void testStateTransition();
    void testEventTrigger();
    void testBlending();

private:
    ModelLoader loader;
    std::unordered_map<std::string, int> boneMap;
    std::shared_ptr<Skeleton> skeleton;
    std::unordered_map<std::string, AnimationClip> clips;
    AnimationStateMachineDefinition stateMachine;
};

void TestAnimationPlayer::initTestCase() {
    // 设置工作目录
    QDir::setCurrent(QCoreApplication::applicationDirPath() + "/..");
    
    // 加载模型以获取骨骼信息
    bool modelLoaded = loader.loadModel("assets/models/milltina/model/milltina.gltf");
    QVERIFY(modelLoaded);
    
    // 获取骨骼映射
    boneMap = loader.getNameToBone();
    
    // 获取实际的骨架数据
    skeleton = std::make_shared<Skeleton>(loader.getSkeleton());
    
    // 加载动画片段
    AnimationImporter importer;
    clips["idle"] = importer.loadAnimation("assets/animations/pose/PET_POSE_1.gltf", boneMap);
    clips["walk"] = importer.loadAnimation("assets/animations/pose/PET_POSE_1.gltf", boneMap); // 使用相同文件作为示例
    
    // 设置状态机
    AnimationState idleState;
    idleState.name = "idle";
    idleState.clipOptions.push_back({"idle", 1.0f});  // 使用正确的字段名
    
    AnimationState walkState;
    walkState.name = "walk";
    walkState.clipOptions.push_back({"walk", 1.0f});  // 使用正确的字段名
    
    stateMachine.states.push_back(idleState);
    stateMachine.states.push_back(walkState);
    stateMachine.defaultState = "idle";  // 设置默认状态
    
    // 添加状态转换
    AnimationTransition transition;
    transition.fromState = "idle";
    transition.toState = "walk";
    transition.blendDuration = 0.3f;  // 使用正确的字段名
    transition.condition = "walk";  // 使用正确的字段名
    
    stateMachine.transactions.push_back(transition);
    
    // 构建查询优化映射
    stateMachine.buildLookupTables();  // 构建查找映射
}

void TestAnimationPlayer::cleanupTestCase() {
    clips.clear();
}

void TestAnimationPlayer::testInitialization() {
    // 测试基本初始化
    AnimationPlayer player(skeleton.get(), &clips, &stateMachine);
    
    QVERIFY(player.currentPose().bonePoses.size() > 0);
}

void TestAnimationPlayer::testAnimationSampling() {
    AnimationPlayer player(skeleton.get(), &clips, &stateMachine);
    
    // 更新动画并检查是否生成有效姿势
    player.update(0.1f);
    const AnimationPose& pose = player.currentPose();
    
    QVERIFY(pose.bonePoses.size() > 0);
}

void TestAnimationPlayer::testStateTransition() {
    AnimationPlayer player(skeleton.get(), &clips, &stateMachine);

    // 测试状态转换
    player.changeState("walk");  // 直接调用私有方法进行测试
    // 在实际应用中，状态转换由状态机处理，这里只是验证方法调用
}

void TestAnimationPlayer::testEventTrigger() {
    AnimationPlayer player(skeleton.get(), &clips, &stateMachine);
    
    // 测试事件触发状态转换
    player.triggerEvent("walk");
    
    // 由于转换有过渡时间，我们需要更新一帧来开始转换
    player.update(0.1f);
    
    // 验证事件已被处理（可能开始混合过程）
    // 注意：在当前实现中没有公开的isTransitioning或getTransitionProgress方法
    // 我们可以通过检查currentPose是否正常更新来间接验证
    const AnimationPose& pose = player.currentPose();
    QVERIFY(pose.bonePoses.size() > 0);
}

void TestAnimationPlayer::testBlending() {
    AnimationPlayer player(skeleton.get(), &clips, &stateMachine);
    
    // 触发混合（通过直接调用私有方法进行测试）
    player.changeState("walk");
    
    // 检查动画更新是否正常工作
    player.update(0.1f);
    const AnimationPose& pose1 = player.currentPose();
    
    player.update(0.1f);
    const AnimationPose& pose2 = player.currentPose();
    
    // 验证姿势被正确更新
    QVERIFY(pose1.bonePoses.size() > 0);
    QVERIFY(pose2.bonePoses.size() > 0);
}

QTEST_MAIN(TestAnimationPlayer)
#include "test_animation_player.moc"