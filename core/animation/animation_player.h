//
// Created by Inoriac on 2025/11/20.
//

#ifndef DESKTOP_PET_ANIMATION_PLAYER_H
#define DESKTOP_PET_ANIMATION_PLAYER_H

#pragma once
#include <string>
#include <memory>
#include <QObject>
#include <random>
#include <unordered_map>
#include <vector>
#include <QStringList>
#include <QVector2D>

#include "animation_types.h"
#include "model_types.h"
#include "animation_crossfader.h"
// #include "animation_state_machine.h"

/**
 * 动画播放�?
 * 播放 AnimationClip, 进行插值，计算当前骨骼姿势
 * 实现动画混合
 * 与状态机联动，处理过渡条�?
 */
class AnimationPlayer : public QObject {
    Q_OBJECT
public:
    explicit AnimationPlayer(Skeleton skeleton,
                    const std::unordered_map<std::string, AnimationClip>* clips,
                    const AnimationStateMachineDefinition* stateMachine);
    ~AnimationPlayer() = default;

    // 更新动画
    void update(double deltaTime);

    // 外部事件触发状态跳�?
    void triggerEvent(const std::string& eventName);

    // 获取最终的当前姿势(用于 skinning)
    const AnimationPose& currentPose() const { return poseFinal; }

    // 获取当前帧数所有骨骼的最终变换矩�?(用于 Shader)
    std::vector<QMatrix4x4> getCurrentTransforms();

    // 获取当前所有骨骼的全局变换矩阵
    const std::vector<QMatrix4x4>& getGlobalTransforms() const { return cachedGlobalTransforms; }

    // 获取骨骼结构定义，用于查询骨骼名称对应的索引
    const Skeleton& getSkeleton() const { return mySkeleton; }

    // 获取当前状态名
    std::string getCurrentStateName() const { return currentStateName; }

    std::string getCurrentClipName() const { return currentClip ? currentClip->name : "None"; }

    // 鼠标追踪输入（屏幕空间方向，范围 [-1,1]，以头部中心->光标向量归一化得到）
    void setScreenLookVector(const QVector2D& screenLookVector);
    void setMouseTrackingEnabled(bool enabled) { mouseTrackingEnabled = enabled; }
    
    // 根据状态名切换动画
    void changeState(const std::string& targetState, double transitionDuration = 0.2);

private:
    // 内部流程函数
    // 依据时间�?AnimationClip 中采�?pose
    void sampleClip(const AnimationClip& clip, double time, AnimationPose& outPose);

    // 根据关键帧数组进行插值（Vec3�?
    QVector3D sampleVec3(const std::vector<KeyFrameVec3>& keys, double time);

    // 插值旋转（Quat�?
    QQuaternion sampleQuat(const std::vector<KeyFrameQuat>& keys, double time);

    // 随机挑�?clip
    void selectRandomClipForState(const AnimationState& state);

    // 在动画采样后附加鼠标追踪旋转层
    void applyMouseTracking(double deltaTime);
    void resolveMouseTrackingBones();
    int findBoneIndexByKeywords(const QStringList& keywords) const;
    void applyLookOffsetToBone(int boneIndex, float yawDeg, float pitchDeg, float rollDeg = 0.0f, float blend = 1.0f);

private:
    // 输入依赖
    Skeleton mySkeleton;
    const std::unordered_map<std::string, AnimationClip>* myClips = nullptr;
    const AnimationStateMachineDefinition* myStateMachine = nullptr;

    // 当前状�?
    std::string currentStateName;
    const AnimationClip* currentClip = nullptr;
    double currentTime = 0;   // 当前动画播放到的秒数
    int currentClipIndex = -1;

    // 混合过渡
    AnimationCrossfader m_crossfader;
    
    AnimationPose poseCurrent;      // 当前姿势
    AnimationPose poseFinal;        // 最终输出姿势

    // 缓存每一帧计算出的全局变换
    std::vector<QMatrix4x4> cachedGlobalTransforms;

    // 鼠标追踪状态
    bool mouseTrackingEnabled = true;
    QVector2D screenLookVector {0.0f, 0.0f};

    int headBoneIndex = -1;
    int spineBoneIndex = -1;
    int chestBoneIndex = -1;
    int upperChestBoneIndex = -1;
    int leftEyeBoneIndex = -1;
    int rightEyeBoneIndex = -1;

    bool mouseTrackingBonesResolved = false;

    float smoothedHeadYaw = 0.0f;
    float smoothedHeadPitch = 0.0f;
    float smoothedSpineYaw = 0.0f;
    float smoothedEyeYaw = 0.0f;
    float smoothedEyePitch = 0.0f;

    std::mt19937 rng { std::random_device{}() };    // 随机数生成器
};


#endif //DESKTOP_PET_ANIMATION_PLAYER_H
