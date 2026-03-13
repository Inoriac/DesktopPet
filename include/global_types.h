//
// Created by Inoriac on 2025/10/15.
//

#ifndef DESKTOP_PET_GLOBAL_TYPES_H
#define DESKTOP_PET_GLOBAL_TYPES_H

#include <string>
#include <QVector3D>
#include <vector>

struct BoneCollider {
    std::string boneName;   // 绑定骨骼名称
    float radius;           // 碰撞半径
    QVector3D offset;       // 相对于骨骼中心的偏移
    std::string tag;        // 交互标签，用于逻辑判断，如 Head 等

    BoneCollider() : radius(0.1f) {}
    BoneCollider(std::string name, float r, QVector3D off, std::string t)
        : boneName(name), radius(r), offset(off), tag(t) {}
};


#endif //DESKTOP_PET_GLOBAL_TYPES_H