//
// Created by Inoriac on 2025/10/22.
//

#include "pet.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonArray>
#include <QStandardPaths>
#include <QDir>

Pet& Pet::instance() {
    static Pet inst;
    return inst;
}

QString Pet::getDataPath() const {
    QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir dir(appData);
    if (!dir.exists()) {
        dir.mkpath(".");
    }
    return appData + "/pets.json";
}

void Pet::load() {
    QString path = getDataPath();
    QFile file(path);

    if (!file.exists()) {
        // 首次运行，添加默认桌宠
        pets["Milltina"] = "assets/models/milltina/model/milltina.gltf";
        save();
        qDebug() << "Created default pet data at:" << path;
        return;
    }

    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Failed to open pet data file:" << path;
        return;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isArray()) {
        qWarning() << "Invalid pet data format";
        return;
    }

    pets.clear();
    QJsonArray arr = doc.array();
    for (const auto& val : arr) {
        if (val.isObject()) {
            QJsonObject obj = val.toObject();
            QString name = obj["name"].toString();
            QString modelPath = obj["modelPath"].toString();
            if (!name.isEmpty() && !modelPath.isEmpty()) {
                pets[name] = modelPath;
            }
        }
    }

    qDebug() << "Loaded" << pets.size() << "pets from:" << path;
}

void Pet::save() {
    QString path = getDataPath();
    QFile file(path);

    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "Failed to save pet data file:" << path;
        return;
    }

    QJsonArray arr;
    for (auto it = pets.begin(); it != pets.end(); ++it) {
        QJsonObject obj;
        obj["name"] = it.key();
        obj["modelPath"] = it.value();
        arr.append(obj);
    }

    QJsonDocument doc(arr);
    file.write(doc.toJson());
    file.close();

    qDebug() << "Saved" << pets.size() << "pets to:" << path;
}

void Pet::addPet(const QString& name, const QString& modelPath) {
    pets[name] = modelPath;
    save();
}

void Pet::removePet(const QString& name) {
    pets.remove(name);
    save();
}

QString Pet::getModelPath(const QString &name) {
    QString path = pets.key(name);
    if (path.isEmpty()) {
        path = "assets/models/" + name + "/model/" + name + ".gltf";
        pets[name] = path;
    }

    return path;
}
