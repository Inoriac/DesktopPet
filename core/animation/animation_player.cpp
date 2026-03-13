//
// Created by Inoriac on 2025/11/20.
//

#include "animation_player.h"

#include <random>

AnimationPlayer::AnimationPlayer(Skeleton skeleton, const std::unordered_map<std::string, AnimationClip> *clips,
                                 const AnimationStateMachineDefinition *stateMachine)
    : mySkeleton(std::move(skeleton)), myClips(clips), myStateMachine(stateMachine), currentClip(nullptr), currentClipIndex(-1){
    size_t boneCount = mySkeleton.bones.size();

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
    if (clipOptions.empty() || !myClips) {
        currentClipIndex = -1;
        currentClip = nullptr;
        return;
    }

    std::vector<float> clipWeights;
    for (const auto& clipOption : clipOptions) {  // 使用引用避免复制
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
    } else {
        currentClipIndex = -1;
        currentClip = nullptr;
    }
}

void AnimationPlayer::update(double deltaTime) {
    if (!currentClip || !myStateMachine) return;  // 添加安全检查

    currentTime += deltaTime;
    double clipLen = currentClip->duration;

    bool shouldLoop = true;
    
    // 获取当前状态配置（循环等属性）
    auto stateIt = myStateMachine->stateIndexMap.find(currentStateName);
    if (stateIt != myStateMachine->stateIndexMap.end()) {
        shouldLoop = myStateMachine->states[stateIt->second].loop;
    }

    // 处理动画时间循环或非循环结束情况
    if (currentTime >= clipLen) {
        if (shouldLoop) {
            // 循环播放时重置时间
            currentTime = std::fmod(currentTime, clipLen);
        } else {
            // 非循环动画结束时，查找自动转换
            bool hasAutoTransition = false;
            
            // 查找当前状态的所有可能转换
            auto transIt = myStateMachine->transactionMap.find(currentStateName);
            if (transIt != myStateMachine->transactionMap.end() && !transIt->second.empty()) {
                for (int transIndex : transIt->second) {
                    // 确保转换索引有效
                    if (transIndex >= 0 && transIndex < static_cast<int>(myStateMachine->transactions.size())) {
                        const AnimationTransition& trans = myStateMachine->transactions[transIndex];
                        
                        // 空条件表示自动转换
                        if (trans.condition.empty()) {
                            // 验证目标状态是否存在
                            if (myStateMachine->stateIndexMap.find(trans.toState) != myStateMachine->stateIndexMap.end()) {
                                blendDuration = trans.blendDuration;
                                changeState(trans.toState);
                                hasAutoTransition = true;
                                break;
                            }
                        }
                    }
                }
            }

            // 没有自动转换时，将时间设置为动画末尾
            if (!hasAutoTransition) {
                currentTime = clipLen;
            }
        }
    }

    // 采样当前动画姿势
    sampleClip(*currentClip, currentTime, poseCurrent);

    // 更新混合时间
    if (isBlending) {
        blendTime += deltaTime;

        if (blendTime >= blendDuration) {
            isBlending = false;
            nextClip = nullptr;
        } else if (nextClip) {
            // 采样下一个动画姿势用于混合
            AnimationPose nextPose(poseCurrent.bonePoses.size());
            double nextClipTime = blendTime * (nextClip->duration / blendDuration);
            sampleClip(*nextClip, nextClipTime, nextPose);
            
            // 更新最终姿势，使用当前姿势和下一姿势进行混合
            size_t boneCount = poseCurrent.bonePoses.size();
            poseFinal.bonePoses.resize(boneCount);
            
            // 计算混合因子
            double t = blendTime / blendDuration;
            t = std::clamp(t, 0.0, 1.0);
            
            // 执行混合计算
            for (size_t i = 0; i < boneCount; ++i) {
                // 对每根骨骼进行混合
                poseFinal.bonePoses[i].translation = posePrevious.bonePoses[i].translation * (1.0 - t) + nextPose.bonePoses[i].translation * t;
                poseFinal.bonePoses[i].scale = posePrevious.bonePoses[i].scale * (1.0 - t) + nextPose.bonePoses[i].scale * t;
                poseFinal.bonePoses[i].rotation = QQuaternion::slerp(posePrevious.bonePoses[i].rotation, nextPose.bonePoses[i].rotation, t);
            }
            
            // 如果混合已完成，关闭混合模式
            if (t >= 1.0) {
                isBlending = false;
                nextClip = nullptr;
                poseFinal = nextPose;  // 完全切换到新姿势
            }
            
            return;  // 已处理混合，直接返回
        }
    }

    // 不是混合状态，直接使用当前姿势
    updateFinalPose();
}

void AnimationPlayer::triggerEvent(const std::string &eventName) {
    if (!myStateMachine || eventName.empty()) return;  // 添加安全检查
    
    // 查找当前状态的所有转换
    auto transIt = myStateMachine->transactionMap.find(currentStateName);
    if (transIt == myStateMachine->transactionMap.end()) return;
    
    // 遍历所有可能的转换，寻找匹配事件条件的转换
    for (int transIndex : transIt->second) {
        const AnimationTransition& trans = myStateMachine->transactions[transIndex];
        // 检查转换条件是否匹配当前事件
        if (trans.condition == eventName) {
            // 设置混合时间并执行状态转换
            blendDuration = trans.blendDuration;
            changeState(trans.toState);
            return;  // 找到匹配的转换后立即返回
        }
    }
}

void AnimationPlayer::sampleClip(const AnimationClip& clip, double time, AnimationPose& outPose) {
    outPose.bonePoses.resize(mySkeleton.bones.size());

    // 对每根骨骼进行处理
    for (const auto& channel : clip.channels) {
        int boneIndex = channel.boneIndex;
        if (boneIndex < 0 || boneIndex >= static_cast<int>(mySkeleton.bones.size())) continue;

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

    // 确保混合状态下姿势大小一致
    if (posePrevious.bonePoses.size() != poseCurrent.bonePoses.size()) {
        poseFinal = poseCurrent;
        isBlending = false;
        nextClip = nullptr;
        return;
    }

    // 计算插值因子(0~1)
    double t = blendDuration > 0 ? (blendTime / blendDuration) : 1.0;
    t = std::clamp(t, 0.0, 1.0);    // 限制t的范围为[0,1]

    size_t boneCount = poseCurrent.bonePoses.size();
    poseFinal.bonePoses.resize(boneCount);

    for (size_t i = 0; i < boneCount; ++i) {
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
    if (!myStateMachine) return;  // 添加安全检查

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
    if (currentClipIndex < 0 || currentClipIndex >= static_cast<int>(target.clipOptions.size())) {
        qDebug() << "[AnimationPlayer] clip index invalid for state" << targetState.c_str();
        return;
    }

    const auto& clipOption = target.clipOptions[currentClipIndex];
    if (!myClips) return;
    auto clipIt = myClips->find(clipOption.clipName);
    if (clipIt == myClips->end()) {
        qDebug() << "[AnimationPlayer] Clip " << clipOption.clipName.c_str() << " not found";
        return;
    }

    // 设置 nextClip 和混合参数
    nextClip = &clipIt->second;
    nextStateName = targetState;

    blendTime = 0.0;
    isBlending = (blendDuration > 0);

    // 切换当前状态
    currentStateName = targetState;
    currentClip = nextClip;
    currentTime = 0.0;
}

std::vector<QMatrix4x4> AnimationPlayer::getCurrentTransforms() {
    size_t boneCount = mySkeleton.bones.size();
    if (boneCount == 0) return {};

    std::vector<QMatrix4x4> finalMatrices(boneCount);

    // 安全检查：如果骨骼姿势未初始化（例如刚启动），返回单位矩阵以防渲染错误
    if (poseFinal.bonePoses.empty()) {
        for(size_t i=0; i<boneCount; ++i) finalMatrices[i].setToIdentity();
        return finalMatrices;
    }

    // 计算骨骼的全局变换 (递归/层级累积)
    // 思路：Local(T*R*S) -> Accumulate Parent -> Global
    if (cachedGlobalTransforms.size() != boneCount) {
        cachedGlobalTransforms.resize(boneCount);
    }

    for (size_t i = 0; i < boneCount; ++i) {
        const auto& boneNode = mySkeleton.bones[i];

        // 构建局部变换矩阵 (T * R * S)
        QMatrix4x4 localTransform;
        localTransform.translate(poseFinal.bonePoses[i].translation);
        localTransform.rotate(poseFinal.bonePoses[i].rotation);
        localTransform.scale(poseFinal.bonePoses[i].scale);

        int parentIdx = boneNode.parent;
        if (parentIdx != -1) {
            // 父变换 * 局部变换
            cachedGlobalTransforms[i] = cachedGlobalTransforms[parentIdx] * localTransform;
        } else {
            // 根骨骼
            cachedGlobalTransforms[i] = localTransform;
        }
    }

    // 计算最终蒙皮矩阵: GlobalTransform * InverseBindMatrix
    for (size_t i = 0; i < boneCount; ++i) {
        finalMatrices[i] = cachedGlobalTransforms[i] * mySkeleton.bones[i].inverseBindMatrix;
    }

    return finalMatrices;
}