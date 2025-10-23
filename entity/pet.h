//
// Created by Inoriac on 2025/10/22.
//

#ifndef DESKTOP_PET_PET_H
#define DESKTOP_PET_PET_H

#include <QString>
#include <QVector>
#include <QJsonObject>

class Pet {
public:
    static Pet& instance();

    // 加载和保存数据
    void load();
    void save();

    // 管理桌宠列表
    QStringList getPetNames() const { return pets.keys(); }
    void addPet(const QString& name, const QString& modelPath);
    void removePet(const QString& name);
    bool hasPet(const QString& name) const { return pets.contains(name); }

    // 根据名称获取模型路径
    QString getModelPath(const QString& name);
private:
    Pet() = default;
    QHash<QString, QString> pets;  // name -> modelPath
    QString getDataPath() const;
};


#endif //DESKTOP_PET_PET_H