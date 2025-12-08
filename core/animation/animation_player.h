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

#include "animation_structs.h"
#include "model_loader.h"
// #include "animation_state_machine.h"

// 一个骨骼的最终姿势
struct BonePose {
    QVector3D translation {0, 0, 0};
    QQuaternion rotation {1, 0, 0, 0};
    QVector3D scale {1, 1, 1};
};

// 某帧下所有骨骼的姿势合集
struct AnimationPose
{
    std::vector<BonePose> bonePoses;

    AnimationPose() = default;
    AnimationPose(size_t boneCount)
        : bonePoses(boneCount)
    {}
};

/**
 * 动画播放器
 * 播放 AnimationClip, 进行插值，计算当前骨骼姿势
 * 实现动画混合
 * 与状态机联动，处理过渡条件
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

    // 外部事件触发状态跳转
    void triggerEvent(const std::string& eventName);

    // 获取最终的当前姿势(用于 skinning)
    const AnimationPose& currentPose() const { return poseFinal; }
    
    // 根据状态名切换动画
    void changeState(const std::string& targetState);

private:
    // 内部流程函数
    // 依据时间从 AnimationClip 中采样 pose
    void sampleClip(const AnimationClip& clip, double time, AnimationPose& outPose);

    // 根据关键帧数组进行插值（Vec3）
    QVector3D sampleVec3(const std::vector<KeyFrameVec3>& keys, double time);

    // 插值旋转（Quat）
    QQuaternion sampleQuat(const std::vector<KeyFrameQuat>& keys, double time);

    // 计算最终姿势（当前动画 + 混合）
    void updateFinalPose();

    // 根据状态名切换动画
    // void changeState(const std::string& targetState);

    // 随机挑选 clip
    void selectRandomClipForState(const AnimationState& state);

private:
    // 输入依赖
    Skeleton mySkeleton;
    const std::unordered_map<std::string, AnimationClip>* myClips = nullptr;
    const AnimationStateMachineDefinition* myStateMachine = nullptr;

    // 当前状态
    std::string currentStateName;
    const AnimationClip* currentClip = nullptr;
    double currentTime = 0;   // 当前动画播放到的秒数
    int currentClipIndex = -1;

    // 混合过渡
    bool isBlending = false;
    std::string nextStateName;
    const AnimationClip* nextClip = nullptr;
    double blendDuration = 0;
    double blendTime = 0;
    int nextClipIndex = -1;

    AnimationPose poseCurrent;      // 当前姿势
    AnimationPose posePrevious;     // 上一帧姿势
    AnimationPose poseFinal;

    std::mt19937 rng { std::random_device{}() };    // 随机数生成器
};


#endif //DESKTOP_PET_ANIMATION_PLAYER_H