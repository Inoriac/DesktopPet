//
// Created by Inoriac on 2025/11/20.
//

#include <QTest>

#include "animation/animation_importer.h"
#include "model_loader.h"

class TestAnimationImporter : public QObject {
    Q_OBJECT

private slots:
    void testLoadAnimation();
    void initTestCase();
};

static ModelLoader loader;
static std::unordered_map<std::string, int> boneMap;

void TestAnimationImporter::initTestCase() {
    QDir::setCurrent(QCoreApplication::applicationDirPath() + "/..");
}


void TestAnimationImporter::testLoadAnimation() {
    // 预先加载模型以获取骨骼映射
    bool modelLoaded = loader.loadModel("assets/models/milltina/model/milltina.gltf");
    QVERIFY(modelLoaded);

    boneMap = loader.getNameToBone();

    AnimationImporter importer;
    AnimationClip clip = importer.loadAnimation("assets/animations/pose/PET_POSE_1.gltf", boneMap);

    QVERIFY(!clip.name.empty());
    QVERIFY(clip.duration > 0.0);
    QVERIFY(!clip.channels.empty());

    // 检查某个已知骨骼的动画通道
    AnimationChannel* headChannel = clip.findChannel(boneMap["Head"]);
    QVERIFY(headChannel != nullptr);
    QVERIFY(!headChannel->rotationKeys.empty());
}

QTEST_MAIN(TestAnimationImporter)
#include "test_animation_importer.moc"