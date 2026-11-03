//
// Created by Inoriac on 2025/10/22.
//

#include "pet.h"

#include <QCoreApplication>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonArray>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

#include <utility>

namespace {

void copyWithWritablePermissions(const QString& srcFile, const QString& dstFile) {
    if (QFile::copy(srcFile, dstFile)) {
        QFile::setPermissions(
            dstFile,
            QFile::permissions(dstFile)
                | QFileDevice::ReadOwner | QFileDevice::WriteOwner
                | QFileDevice::ReadUser | QFileDevice::WriteUser);
    }
}

void seedDirectoryFiles(const QString& sourceDir, const QString& targetDir) {
    QDir src(sourceDir);
    if (!src.exists()) return;
    QDir().mkpath(targetDir);
    const QFileInfoList entries = src.entryInfoList(
        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& info : entries) {
        const QString name = info.fileName();
        if (name.endsWith(QStringLiteral(".lock"))
            || name.endsWith(QStringLiteral("-wal"))
            || name.endsWith(QStringLiteral("-shm"))) {
            continue;
        }
        const QString dstPath = QDir(targetDir).filePath(name);
        if (info.isDir()) {
            seedDirectoryFiles(info.absoluteFilePath(), dstPath);
        } else if (!QFileInfo::exists(dstPath)) {
            copyWithWritablePermissions(info.absoluteFilePath(), dstPath);
            if (name.endsWith(QStringLiteral(".db"))
                || name.endsWith(QStringLiteral(".sqlite"))
                || name.endsWith(QStringLiteral(".sqlite3"))) {
                for (const QString& suffix : {QStringLiteral("-wal"), QStringLiteral("-shm")}) {
                    const QString srcSidecar = info.absoluteFilePath() + suffix;
                    const QString dstSidecar = dstPath + suffix;
                    if (QFileInfo::exists(srcSidecar) && !QFileInfo::exists(dstSidecar)) {
                        copyWithWritablePermissions(srcSidecar, dstSidecar);
                    }
                }
            }
        }
    }
}

} // namespace

Pet& Pet::instance() {
    static Pet inst;
    return inst;
}

QString Pet::resolveAppDataRoot() {
    const QString primary = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!primary.trimmed().isEmpty()) {
        QDir dir(primary);
        if (!dir.exists()) {
            dir.mkpath(QStringLiteral("."));
        }
        const QString probePath = dir.filePath(
            QStringLiteral(".dp-probe-%1-%2.tmp")
                .arg(QCoreApplication::applicationPid())
                .arg(QUuid::createUuid().toString(QUuid::Id128)));
        QFile probe(probePath);
        if (probe.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            probe.close();
            QFile::remove(probePath);
            return primary;
        }
    }
    const QString fallback = QDir::current().absoluteFilePath(QStringLiteral("runtime/appdata"));
    QDir().mkpath(fallback);
    {
        const QString tempDir = QDir::tempPath();
        const QString tempProbePath = QDir(tempDir).filePath(
            QStringLiteral(".dp-tmp-probe-%1-%2.tmp")
                .arg(QCoreApplication::applicationPid())
                .arg(QUuid::createUuid().toString(QUuid::Id128)));
        QFile tempProbe(tempProbePath);
        if (tempProbe.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            tempProbe.close();
            QFile::remove(tempProbePath);
        } else {
            const QString fallbackTmp = QDir(fallback).filePath(QStringLiteral("tmp"));
            QDir().mkpath(fallbackTmp);
            qputenv("TMP", QDir::toNativeSeparators(fallbackTmp).toUtf8());
            qputenv("TEMP", QDir::toNativeSeparators(fallbackTmp).toUtf8());
        }
    }
    if (!primary.trimmed().isEmpty() && QDir(primary).exists()) {
        seedDirectoryFiles(primary, fallback);
    }
    return fallback;
}

QString Pet::getDataPath() const {
    const QString appData = resolveAppDataRoot();
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
