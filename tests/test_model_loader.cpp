#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include "model_loader.h"

class TestModelLoader : public QObject {
    Q_OBJECT

private slots:
    void acceptsMinimalScene();
    void rejectsModelWithoutScenes();
    void rejectsOutOfBoundsAccessor();
    void rejectsAccessorOutsideBufferView();
};

namespace {
QString writeModel(QTemporaryDir& dir, const QString& fileName, const QByteArray& json) {
    const QString path = dir.filePath(fileName);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    if (file.write(json) != json.size()) return {};
    file.close();
    return path;
}
}

void TestModelLoader::acceptsMinimalScene() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeModel(
        dir,
        "minimal.gltf",
        R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[]}]})");
    QVERIFY(!path.isEmpty());

    ModelLoader loader;
    QVERIFY(loader.loadModel(path.toStdString()));
}

void TestModelLoader::rejectsModelWithoutScenes() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeModel(dir, "no-scenes.gltf", R"({"asset":{"version":"2.0"}})");
    QVERIFY(!path.isEmpty());

    ModelLoader loader;
    QVERIFY(!loader.loadModel(path.toStdString()));
}

void TestModelLoader::rejectsOutOfBoundsAccessor() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeModel(
        dir,
        "bad-accessor.gltf",
        R"({
          "asset":{"version":"2.0"},
          "scene":0,
          "scenes":[{"nodes":[0]}],
          "nodes":[{"mesh":0}],
          "buffers":[{"byteLength":4,"uri":"data:application/octet-stream;base64,AAAAAA=="}],
          "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":4}],
          "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}],
          "meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}]
        })");
    QVERIFY(!path.isEmpty());

    ModelLoader loader;
    QVERIFY(!loader.loadModel(path.toStdString()));
}

void TestModelLoader::rejectsAccessorOutsideBufferView() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeModel(
        dir,
        "bad-buffer-view.gltf",
        R"({
          "asset":{"version":"2.0"},
          "scene":0,
          "scenes":[{"nodes":[0]}],
          "nodes":[{"mesh":0}],
          "buffers":[{"byteLength":16,"uri":"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAAAAAA=="}],
          "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":4}],
          "accessors":[{"bufferView":0,"componentType":5126,"count":1,"type":"VEC3"}],
          "meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}]
        })");
    QVERIFY(!path.isEmpty());

    ModelLoader loader;
    QVERIFY(!loader.loadModel(path.toStdString()));
}

QTEST_APPLESS_MAIN(TestModelLoader)
#include "test_model_loader.moc"
