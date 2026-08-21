//
// Created by Inoriac on 2025/10/22.
//

#ifndef DESKTOP_PET_PET_H
#define DESKTOP_PET_PET_H

#include <QString>
#include <QVector>
#include <QJsonObject>

#include "ai/runtime/profile_resolver.h"

class Pet {
public:
    static Pet& instance();

    // C++ 只读 launcher 管理的角色清单。
    bool load(QString* errorMessage = nullptr);

    QStringList getPetNames() const;
    QList<PetProfile> getProfiles() const { return pets; }
    bool hasPet(const QString& name) const;
    std::optional<PetProfile> getProfile(const QString& name) const;

    // 根据名称获取模型路径
    QString getModelPath(const QString& name) const;
private:
    Pet() = default;
    QList<PetProfile> pets;
    QString getDataPath() const;
};


#endif //DESKTOP_PET_PET_H
