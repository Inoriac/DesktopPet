//
// Created by Inoriac on 2025/11/20.
//

#include "animation_player.h"

#include <random>

AnimationPlayer::AnimationPlayer(const Skeleton *skeleton, const std::unordered_map<std::string, AnimationClip> *clips,
                                 const AnimationStateMachineDefinition *stateMachine)
    : mySkeleton(skeleton), myClips(clips), myStateMachine(stateMachine), currentClip(nullptr){
    size_t boneCount = mySkeleton ? mySkeleton->bones.size() : 0;

    poseCurrent = AnimationPose(boneCount);
    posePrevious = AnimationPose(boneCount);
    poseFinal = AnimationPose(boneCount);

    // 默认状态
    if (myStateMachine && !myStateMachine->defaultState.empty()) {
        currentStateName = myStateMachine->defaultState;
        currentTime = 0.0;

        auto stateIt = myStateMachine->stateIndexMap.find(currentStateName);
        if (stateIt != myStateMachine->stateIndexMap.end()) {
            const AnimationState& st = myStateMachine->states[stateIt->second];
            selectRandomClipForState(st);
        }
    }
}

void AnimationPlayer::selectRandomClipForState(const AnimationState& state) {
    const auto& clipOptions = state.clipOptions;
    if (clipOptions.empty()) {
        currentClipIndex = -1;
        currentClip = nullptr;
        return;
    }

    std::vector<float> clipWeights;
    for (const auto clipOption : clipOptions) {
        clipWeights.push_back(clipOption.weight);
    }

    std::discrete_distribution<int> dist(clipWeights.begin(), clipWeights.end());
    currentClipIndex = dist(rng);

    // 设置 currentClip 指针
    if (currentClipIndex >= 0 && currentClipIndex < static_cast<int>(clipOptions.size())) {
        const auto& clipName = clipOptions[currentClipIndex].clipName;
        auto clipIt = myClips->find(clipName);
        if (clipIt != myClips->end()) {
            currentClip = &clipIt->second;
        } else {
            currentClip = nullptr;
        }
    }
}

void AnimationPlayer::update(double deltaTime) {
    if (currentClip == nullptr) return;

    currentTime += deltaTime;
    double clipLen = currentClip->duration;


}

void AnimationPlayer::triggerEvent(const std::string &eventName) {
}

void AnimationPlayer::sampleClip(const AnimationClip& clip, double time, AnimationPose& outPose) {
    outPose.bonePoses.resize(mySkeleton->bones.size());

    // 对每根骨骼进行处理
    for (const auto& channel : clip.channels) {
        int boneIndex = channel.boneIndex;
        if (boneIndex < 0 || boneIndex >= mySkeleton->bones.size()) continue;

        BonePose& bp = outPose.bonePoses[boneIndex];

        if (channel.hasTranslation())
            bp.translation = sampleVec3(channel.translationKeys, time);

        if (channel.hasRotation())
            bp.rotation = sampleQuat(channel.rotationKeys, time);

        if (channel.hasScale())
            bp.scale = sampleVec3(channel.scaleKeys, time);
    }
}

QVector3D AnimationPlayer::sampleVec3(const std::vector<KeyFrameVec3>& keys, double time){
    if (keys.empty()) return QVector3D();
    if (keys.size() == 1) return QVector3D(keys[0].x, keys[0].y, keys[0].z);

    // 若时间超过最后一个关键点，直接取最后
    if (time >= keys.back().time) {
        const auto& k = keys.back();
        return QVector3D(k.x, k.y, k.z);
    }

    // 寻找当前时间 time 所在的关键帧区间
    for (size_t i = 0; i < keys.size() - 1; ++i) {
        const auto& k1 = keys[i];
        const auto& k2 = keys[i+1];

        if (time >= k1.time && time <= k2.time) {
            double t = (time - k1.time) / (k2.time - k1.time);
            return QVector3D(
                k1.x + (k2.x - k1.x) * t,
                k1.y + (k2.y - k1.y) * t,
                k1.z + (k2.z - k1.z) * t);
        }
    }

    // 理论上不会走到这里，保险措施
    const auto& k = keys.back();
    return QVector3D(k.x, k.y, k.z);
}

QQuaternion AnimationPlayer::sampleQuat(const std::vector<KeyFrameQuat>& keys, double time){
    if (keys.empty()) return QQuaternion(1, 0, 0, 0);
    if (keys.size() == 1)
        return QQuaternion(keys[0].w, keys[0].x, keys[0].y, keys[0].z);

    if (time >= keys.back().time) {
        const auto& k = keys.back();
        return QQuaternion(k.w, k.x, k.y, k.z);
    }

    for (size_t i = 0; i < keys.size() - 1; i++) {
        const auto& k1 = keys[i];
        const auto& k2 = keys[i + 1];

        if (time >= k1.time && time <= k2.time) {
            double t = (time - k1.time) / (k2.time - k1.time);
            QQuaternion q1(k1.w, k1.x, k1.y, k1.z);
            QQuaternion q2(k2.w, k2.x, k2.y, k2.z);
            return QQuaternion::slerp(q1, q2, t);
        }
    }

    const auto& k = keys.back();
    return QQuaternion(k.w, k.x, k.y, k.z);
}

void AnimationPlayer::updateFinalPose(){    // TODO:暂时采用相对比较简单的混合方式进行动作混合
    // 若不在混合，直接使用当前姿势
    if (!isBlending) {
        poseFinal = poseCurrent;
        return;
    }

    // 计算插值因子(0~1)
    double t = blendDuration > 0 ? (blendTime / blendDuration) : 1.0;
    t = std::clamp(t, 0.0, 1.0);    // 限制t的范围为[0,1]

    size_t boneCount = poseCurrent.bonePoses.size();
    poseFinal.bonePoses.resize(boneCount);

    for (size_t i = 0; i< boneCount; ++i) {
        const BonePose& a = posePrevious.bonePoses[i];  // 来源A
        const BonePose& b = poseCurrent.bonePoses[i];   // 来源B
        BonePose& out = poseFinal.bonePoses[i];

        // T/S 使用线性插值
        out.translation = a.translation * (1.0 - t) + b.translation * t;
        out.scale = a.scale * (1.0 - t) + b.scale * t;

        // 旋转使用 slerp
        out.rotation = QQuaternion::slerp(a.rotation, b.rotation, t);
    }

    // 若 blending 完成，关闭混合模式
    if (t >= 1.0) {
        isBlending = false;
        nextClip = nullptr;
    }
}

void AnimationPlayer::changeState(const std::string& targetState){
    if (currentStateName == targetState) return;

    // 查询目标状态
    auto it = myStateMachine->stateIndexMap.find(targetState);
    if (it == myStateMachine->stateIndexMap.end()) {
        qDebug() << "[AnimationPlayer] State " << targetState << " not found";
        return;
    }

    int stateIndex = it->second;
    const AnimationState& target = myStateMachine->states[stateIndex];

    // 保存当前动画姿势(用于混合)
    posePrevious = poseFinal;

    // 选择目标 clip
    selectRandomClipForState(target);
    if (nextClipIndex < 0 || nextClipIndex >= target.clipOptions.size()) {
        qDebug() << "[AnimationPlayer] clip " << targetState.c_str() << " not found";
        return;
    }

    const auto& clipOption = target.clipOptions[nextClipIndex];
    auto clipIt = myClips->find(clipOption.clipName);
    if (clipIt == myClips->end()) {
        qDebug() << "[AnimationPlayer] Clip " << targetState.c_str() << " not found";
        return;
    }

    // 设置 nextClip 和混合参数
    nextClip = &clipIt->second;
    nextStateName = targetState;

    blendTime = 0.0;
    isBlending = (blendDuration > 0);

    // 切换当前状态
    currentStateName = targetState;
    currentClipIndex = nextClipIndex;
    currentClip = nextClip;
    currentTime = 0.0;
}
