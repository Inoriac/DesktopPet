//
// Created by Inoriac on 2025/10/22.
//

#include "pet.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonArray>
#include <QStandardPaths>
#include <QDir>

#include <utility>

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

bool Pet::load(QString* errorMessage) {
    return loadFromPath(getDataPath(), errorMessage);
}

bool Pet::loadFromPath(const QString& path, QString* errorMessage) {
    QFile file(path);

    pets.clear();
    if (!file.exists()) {
        if (errorMessage) *errorMessage = QStringLiteral("pets.json does not exist");
        return false;
    }

    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage) *errorMessage = file.errorString();
        return false;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (errorMessage) *errorMessage = parseError.errorString();
        return false;
    }
    if (!doc.isArray()) {
        if (errorMessage) *errorMessage = QStringLiteral("pets.json root must be an array");
        return false;
    }

    QList<PetProfile> loadedProfiles;
    QJsonArray arr = doc.array();
    if (arr.isEmpty()) {
        if (errorMessage) *errorMessage = QStringLiteral("pets.json must contain a profile");
        return false;
    }
    for (const auto& val : arr) {
        if (!val.isObject()) {
            if (errorMessage) *errorMessage = QStringLiteral("pets.json entry must be an object");
            return false;
        }
        const QJsonObject obj = val.toObject();
        const QJsonValue nameValue = obj.value(QStringLiteral("name"));
        const QJsonValue modelPathValue = obj.value(QStringLiteral("modelPath"));
        const QJsonValue profileIdValue = obj.value(QStringLiteral("profileId"));
        if (!nameValue.isString() || nameValue.toString().trimmed().isEmpty()
            || !modelPathValue.isString() || modelPathValue.toString().trimmed().isEmpty()
            || (!profileIdValue.isUndefined() && !profileIdValue.isString())) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("pets.json entry has invalid name, modelPath, or profileId");
            }
            return false;
        }
        loadedProfiles.append({nameValue.toString(), modelPathValue.toString(),
                               profileIdValue.toString()});
    }

    pets = std::move(loadedProfiles);
    qDebug() << "Loaded" << pets.size() << "pets from:" << path;
    return true;
}

QStringList Pet::getPetNames() const {
    QStringList names;
    for (const PetProfile& profile : pets) names.append(profile.name);
    return names;
}

bool Pet::hasPet(const QString& name) const {
    return getProfile(name).has_value();
}

std::optional<PetProfile> Pet::getProfile(const QString& name) const {
    for (const PetProfile& profile : pets) {
        if (profile.name == name) return profile;
    }
    return std::nullopt;
}

QString Pet::getModelPath(const QString& name) const {
    const std::optional<PetProfile> profile = getProfile(name);
    return profile ? profile->modelPath : QString();
}
