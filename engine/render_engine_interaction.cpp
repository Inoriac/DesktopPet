//
// Created by Inoriac on 2025/10/15.
//

#include "render_engine.h"

#include <QMatrix4x4>
#include <QStringList>
#include <QVector2D>
#include <QVector3D>
#include <QVector4D>

#include <algorithm>
#include <cfloat>
#include <cmath>

void RenderEngine::setTrackingYawInput(float normalizedX) {
    const float x = std::clamp(normalizedX, -1.0f, 1.0f);
    constexpr float kMaxTrackingYawDeg = 28.0f;
    trackingYawTargetDeg = x * kMaxTrackingYawDeg;
}

int RenderEngine::findBoneIndexByKeywords(const Skeleton& skeleton, const QStringList& keywords) const {
    for (const auto& pair : skeleton.nameToIndex) {
        QString lowered = QString::fromStdString(pair.first).toLower();
        for (const QString& key : keywords) {
            if (lowered == key) return pair.second;
        }
    }

    for (const auto& pair : skeleton.nameToIndex) {
        QString lowered = QString::fromStdString(pair.first).toLower();
        for (const QString& key : keywords) {
            if (lowered.contains(key)) return pair.second;
        }
    }

    return -1;
}

bool RenderEngine::getHeadScreenPosition(QVector2D& outViewportPos) {
    if (!animationPlayer || viewportWidth <= 0 || viewportHeight <= 0) return false;

    const Skeleton& skeleton = animationPlayer->getSkeleton();
    const int headBoneIndex = findBoneIndexByKeywords(skeleton, {"head", "j_bip_c_head"});
    if (headBoneIndex < 0) return false;

    // 刷新一遍矩阵缓存，确保 getGlobalTransforms 返回本帧姿态。
    (void)animationPlayer->getCurrentTransforms();
    const auto& boneTransforms = animationPlayer->getGlobalTransforms();
    if (headBoneIndex >= static_cast<int>(boneTransforms.size())) return false;

    QMatrix4x4 proj;
    float aspect = (viewportHeight > 0) ? float(viewportWidth) / float(viewportHeight) : 1.0f;
    proj.perspective(45.0f, aspect, 0.1f, 100.0f);

    QMatrix4x4 view;
    view.lookAt(cameraEye, cameraCenter, QVector3D(0.0f, 1.0f, 0.0f));

    QMatrix4x4 model;
    model.rotate(trackingYawCurrentDeg, 0.0f, 1.0f, 0.0f);
    model.rotate(90.0f, 1.0f, 0.0f, 0.0f);

    float finalScale = modelScale;
    if (finalScale <= 0.0001f) finalScale = 1.0f;
    model.scale(finalScale);
    model.rotate(angleDeg, 0.0f, 1.0f, 0.0f);

    QVector4D headWorld = model * boneTransforms[headBoneIndex] * QVector4D(0.0f, 0.0f, 0.0f, 1.0f);
    QVector4D clip = proj * view * headWorld;
    if (std::fabs(clip.w()) < 1e-6f) return false;

    QVector3D ndc = clip.toVector3D() / clip.w();
    float sx = (ndc.x() * 0.5f + 0.5f) * viewportWidth;
    float sy = (1.0f - (ndc.y() * 0.5f + 0.5f)) * viewportHeight;

    outViewportPos = QVector2D(sx, sy);
    return true;
}

std::string RenderEngine::checkHit(int viewX, int viewY) {
    if (!animationPlayer || boneColliders.empty()) return "";

    // 获取变换矩阵
    QMatrix4x4 view;
    view.lookAt(cameraEye, cameraCenter, QVector3D(0, 1, 0));

    QMatrix4x4 proj;
    float aspect = (viewportHeight > 0) ? float(viewportWidth) / float(viewportHeight) : 1.0f;
    proj.perspective(45.0f, aspect, 0.1f, 100.0f); // 0.1f to match render

    QMatrix4x4 vpMatrix = proj * view;

    int glY = viewportHeight - viewY; // OpenGL 的 Y 轴是反向的

    QVector2D mouseScreen(static_cast<float>(viewX), static_cast<float>(glY));

    // 获取骨骼数据
    const auto& boneTransforms = animationPlayer->getGlobalTransforms();
    const auto& skeleton = animationPlayer->getSkeleton();

    float minDist2 = FLT_MAX;
    std::string hitTag = "";

    // 用于自动将半径从世界空间投影到屏幕空间的辅助 lambda
    auto worldToScreen = [&](const QVector3D& worldPos) -> QVector2D {
        QVector4D clip = vpMatrix * QVector4D(worldPos, 1.0f);
        if (std::fabs(clip.w()) < 1e-6f) return QVector2D(-1e6f, -1e6f); // 避免除以零
        QVector3D ndc = clip.toVector3D() / clip.w();  // 透视除法：clip → NDC
        float sx = (ndc.x() * 0.5f + 0.5f) * viewportWidth;
        float sy = (ndc.y() * 0.5f + 0.5f) * viewportHeight;
        return QVector2D(sx, sy);
    };

    // 相机右方向（用于投影半径）
    QVector3D camRight = QVector3D::crossProduct(
        (cameraCenter - cameraEye).normalized(),
        QVector3D(0, 1, 0)
    ).normalized();

    for (const auto& collider : boneColliders) {
        int boneIndex = -1;
        auto it = skeleton.nameToIndex.find(collider.boneName);
        if (it != skeleton.nameToIndex.end()) {
            boneIndex = it->second;
        } else {
            std::string targetName = collider.boneName;
            for (const auto& pair : skeleton.nameToIndex) {
                const std::string& name = pair.first;
                if (name.size() >= targetName.size() &&
                    name.compare(name.size() - targetName.size(), targetName.size(), targetName) == 0) {
                    if (name.size() == targetName.size()) {
                        boneIndex = pair.second;
                        break;
                    }
                    char separator = name[name.size() - targetName.size() - 1];
                    if (separator == ':' || separator == '_' || separator == ' ') {
                        boneIndex = pair.second;
                        break;
                    }
                }
            }
        }
        if (boneIndex == -1 || boneIndex >= static_cast<int>(boneTransforms.size())) continue;

        // 计算骨骼在世界空间的位置
        // 骨骼本地偏移（跟随骨骼旋转）
        QVector3D boneWorldPos = currentModelMatrix * (boneTransforms[boneIndex] * collider.offset);
        // 加上世界空间固定偏移（不跟随骨骼旋转）
        boneWorldPos += collider.worldOffset;

        // 投影到 2D 屏幕空间
        QVector2D boneScreen = worldToScreen(boneWorldPos);

        // 投影半径：世界空间 hoverRadius 在屏幕上有多大
        QVector2D edgeScreen = worldToScreen(boneWorldPos + camRight * collider.hoverRadius);
        float screenRadius2 = (boneScreen - edgeScreen).lengthSquared();

        // 2D 距离判断
        float dist2 = (mouseScreen - boneScreen).lengthSquared();

        // === DEBUG: 打印每个碰撞体的投影信息 ===
        // float screenRadius = std::sqrt(screenRadius2);
        // float dist = std::sqrt(dist2);
        // qDebug().noquote() << QString("  [HitCheck] %1 (tag:%2) boneIdx:%3 worldPos:(%4,%5,%6) screenPos:(%7,%8) mouse:(%9,%10) screenR:%11px dist:%12px %13")
        //     .arg(collider.boneName.c_str())
        //     .arg(collider.tag.c_str())
        //     .arg(boneIndex)
        //     .arg(boneWorldPos.x(), 0, 'f', 3).arg(boneWorldPos.y(), 0, 'f', 3).arg(boneWorldPos.z(), 0, 'f', 3)
        //     .arg(boneScreen.x(), 0, 'f', 1).arg(boneScreen.y(), 0, 'f', 1)
        //     .arg(mouseScreen.x(), 0, 'f', 1).arg(mouseScreen.y(), 0, 'f', 1)
        //     .arg(screenRadius, 0, 'f', 1)
        //     .arg(dist, 0, 'f', 1)
        //     .arg(dist2 <= screenRadius2 ? "HIT" : "miss");

        if (dist2 <= screenRadius2) {
            // 命中，取最近的（屏幕距离最近 = 最精确匹配）
            if (dist2 < minDist2) {
                minDist2 = dist2;
                hitTag = collider.tag;
            }
        }
    }
    return hitTag;
}

// bool RenderEngine::intersectRaySphere(const QVector3D &rayOrigin, const QVector3D &rayDir, const QVector3D &sphereCenter, float sphereRadius, float &outDist) {
//     QVector3D m = rayOrigin - sphereCenter;
//     float b = QVector3D::dotProduct(m, rayDir);
//     float c = QVector3D::dotProduct(m, m) - sphereRadius * sphereRadius;
//     // 射线起点在球外且指向远离球心的方向
//     if (c > 0.0f && b > 0.0f) return false;
//
//     float discr = b * b - c;
//     if (discr < 0.0f) return false;
//
//     // 计算最近交点
//     float t = -b - sqrt(discr);
//     if (t < 0.0f) t = 0.0f;
//
//     outDist = t;
//     return true;
// }
//
// bool RenderEngine::intersectRayCapsule(const QVector3D &rayOrigin, const QVector3D &rayDir,
//                                        const QVector3D &capsuleA, const QVector3D &capsuleB,
//                                        float capsuleRadius, float &outDist) {
//     QVector3D ab = capsuleB - capsuleA;
//     float abLenSq = QVector3D::dotProduct(ab, ab);
//
//     // 退化为球体
//     if (abLenSq < 1e-8f) {
//         return intersectRaySphere(rayOrigin, rayDir, capsuleA, capsuleRadius, outDist);
//     }
//
//     QVector3D w0 = rayOrigin - capsuleA;
//     float a = QVector3D::dotProduct(rayDir, rayDir); // 理论上为1
//     float b = QVector3D::dotProduct(rayDir, ab);
//     float c = abLenSq;
//     float d = QVector3D::dotProduct(rayDir, w0);
//     float e = QVector3D::dotProduct(ab, w0);
//
//     float denom = a * c - b * b;
//     float tRay = 0.0f;
//     float tSeg = 0.0f;
//
//     if (std::fabs(denom) > 1e-8f) {
//         tRay = (b * e - c * d) / denom;
//         tSeg = (a * e - b * d) / denom;
//     } else {
//         // 近平行：固定在射线起点投影
//         tRay = 0.0f;
//         tSeg = e / c;
//     }
//
//     // 约束到有效区间
//     if (tRay < 0.0f) tRay = 0.0f;
//     if (tSeg < 0.0f) tSeg = 0.0f;
//     if (tSeg > 1.0f) tSeg = 1.0f;
//
//     // 约束 segment 后重新计算 ray 参数
//     tRay = -(d + b * tSeg) / a;
//     if (tRay < 0.0f) {
//         tRay = 0.0f;
//         tSeg = e / c;
//         if (tSeg < 0.0f) tSeg = 0.0f;
//         if (tSeg > 1.0f) tSeg = 1.0f;
//     }
//
//     QVector3D closestRay = rayOrigin + rayDir * tRay;
//     QVector3D closestSeg = capsuleA + ab * tSeg;
//     QVector3D delta = closestRay - closestSeg;
//     float distSq = QVector3D::dotProduct(delta, delta);
//
//     if (distSq <= capsuleRadius * capsuleRadius) {
//         outDist = tRay;
//         return true;
//     }
//
//     return false;
// }

void RenderEngine::setAnimationPlayer(std::unique_ptr<AnimationPlayer> player) {
    animationPlayer = std::move(player);
}

void RenderEngine::updateAnimation(float deltaTime) {
    if (animationPlayer) {
        animationPlayer->update(deltaTime);
    }
}

