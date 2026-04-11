//
// Created by Inoriac on 2025/11/20.
//

#include "animation_player.h"

#include <random>
#include <algorithm>
#include <cmath>

AnimationPlayer::AnimationPlayer(Skeleton skeleton, const std::unordered_map<std::string, AnimationClip> *clips,
                                 const AnimationStateMachineDefinition *stateMachine)
    : mySkeleton(std::move(skeleton)), myClips(clips), myStateMachine(stateMachine), currentClip(nullptr), currentClipIndex(-1){
    size_t boneCount = mySkeleton.bones.size();

    poseCurrent = AnimationPose(boneCount);
    
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

    resolveMouseTrackingBones();
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
                                changeState(trans.toState, trans.blendDuration);
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
    if (m_crossfader.isFading()) {
        m_crossfader.update(deltaTime);
        AnimationCrossfader::blendPoses(m_crossfader.getSnapshot(), poseCurrent, poseFinal, m_crossfader.getBlendWeight());
    } else {
        poseFinal = poseCurrent;
    }

    applyMouseTracking(deltaTime);
}

void AnimationPlayer::setScreenLookVector(const QVector2D& lookVector) {
    screenLookVector.setX(std::clamp(lookVector.x(), -1.0f, 1.0f));
    screenLookVector.setY(std::clamp(lookVector.y(), -1.0f, 1.0f));
}

int AnimationPlayer::findBoneIndexByKeywords(const QStringList& keywords) const {
    auto matches = [&](const std::string& boneName) -> bool {
        QString lowered = QString::fromStdString(boneName).toLower();
        for (const QString& key : keywords) {
            if (lowered.contains(key)) return true;
        }
        return false;
    };

    // 优先精确匹配，尽量减少误识别。
    for (size_t i = 0; i < mySkeleton.bones.size(); ++i) {
        QString lowered = QString::fromStdString(mySkeleton.bones[i].name).toLower();
        for (const QString& key : keywords) {
            if (lowered == key) return static_cast<int>(i);
        }
    }

    for (size_t i = 0; i < mySkeleton.bones.size(); ++i) {
        if (matches(mySkeleton.bones[i].name)) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

void AnimationPlayer::resolveMouseTrackingBones() {
    if (mouseTrackingBonesResolved) return;

    headBoneIndex = findBoneIndexByKeywords({"head", "j_bip_c_head"});
    spineBoneIndex = findBoneIndexByKeywords({"spine", "j_bip_c_spine", "hips"});
    chestBoneIndex = findBoneIndexByKeywords({"chest", "upperbody", "j_bip_c_chest"});
    upperChestBoneIndex = findBoneIndexByKeywords({"upperchest", "spine2", "spine3"});
    leftEyeBoneIndex = findBoneIndexByKeywords({"lefteye", "eye_l", "eye.l", "left_eye"});
    rightEyeBoneIndex = findBoneIndexByKeywords({"righteye", "eye_r", "eye.r", "right_eye"});

    mouseTrackingBonesResolved = true;
}

void AnimationPlayer::applyLookOffsetToBone(int boneIndex, float yawDeg, float pitchDeg, float rollDeg, float blend) {
    if (boneIndex < 0 || boneIndex >= static_cast<int>(poseFinal.bonePoses.size())) return;
    if (blend <= 0.0f) return;

    const QQuaternion delta = QQuaternion::fromEulerAngles(-pitchDeg, yawDeg, rollDeg);
    QQuaternion base = poseFinal.bonePoses[boneIndex].rotation;
    QQuaternion target = delta * base;

    if (blend >= 1.0f) {
        poseFinal.bonePoses[boneIndex].rotation = target;
    } else {
        poseFinal.bonePoses[boneIndex].rotation = QQuaternion::slerp(base, target, blend);
    }
}

void AnimationPlayer::applyMouseTracking(double deltaTime) {
    if (!mouseTrackingEnabled) return;
    if (poseFinal.bonePoses.empty()) return;
    if (!mouseTrackingBonesResolved) resolveMouseTrackingBones();

    const float dt = static_cast<float>(std::max(0.0, deltaTime));

    // 约束策略：头部限制按需求调整为 Yaw 20 / Pitch 14。
    constexpr float kHeadYawLimit = 20.0f;
    constexpr float kHeadPitchLimit = 14.0f;
    constexpr float kTorsoYawLimit = 10.0f;
    constexpr float kEyeYawLimit = 10.0f;
    constexpr float kEyePitchLimit = 10.0f;

    constexpr float kHeadSmoothness = 8.0f;
    constexpr float kSpineSmoothness = 16.0f;
    constexpr float kEyeSmoothness = 12.0f;

    // 输入由“头部中心->光标”屏幕向量提供。
    // 使用一个朝前的 3D 向量做角度解算，避免线性映射导致的平面转动感。
    QVector3D dir(screenLookVector.x(), screenLookVector.y(), 1.15f);
    if (dir.lengthSquared() < 1e-6f) {
        dir = QVector3D(0.0f, 0.0f, 1.0f);
    } else {
        dir.normalize();
    }

    const float yawDeg3D = std::atan2(dir.x(), dir.z()) * 57.2957795f;
    const float pitchDeg3D = std::asin(std::clamp(dir.y(), -1.0f, 1.0f)) * 57.2957795f;

    const float targetHeadYaw = std::clamp(yawDeg3D, -kHeadYawLimit, kHeadYawLimit);
    const float targetHeadPitch = std::clamp(pitchDeg3D, -kHeadPitchLimit, kHeadPitchLimit);
    const float targetTorsoYaw = std::clamp(yawDeg3D, -kTorsoYawLimit, kTorsoYawLimit);
    const float targetEyeYaw = std::clamp(yawDeg3D, -kEyeYawLimit, kEyeYawLimit);
    const float targetEyePitch = std::clamp(pitchDeg3D, -kEyePitchLimit, kEyePitchLimit);

    const float headAlpha = 1.0f - std::exp(-kHeadSmoothness * dt);
    const float spineAlpha = 1.0f - std::exp(-kSpineSmoothness * dt);
    const float eyeAlpha = 1.0f - std::exp(-kEyeSmoothness * dt);

    smoothedHeadYaw += (targetHeadYaw - smoothedHeadYaw) * headAlpha;
    smoothedHeadPitch += (targetHeadPitch - smoothedHeadPitch) * headAlpha;
    smoothedSpineYaw += (targetTorsoYaw - smoothedSpineYaw) * spineAlpha;
    smoothedEyeYaw += (targetEyeYaw - smoothedEyeYaw) * eyeAlpha;
    smoothedEyePitch += (targetEyePitch - smoothedEyePitch) * eyeAlpha;

    // 躯干保持正立：仅进行 yaw 扭转，不做 pitch/roll 倾斜。
    applyLookOffsetToBone(spineBoneIndex,
                          smoothedSpineYaw * 0.12f,
                          0.0f,
                          0.0f,
                          1.0f);
    applyLookOffsetToBone(chestBoneIndex,
                          smoothedSpineYaw * 0.10f,
                          0.0f,
                          0.0f,
                          1.0f);
    applyLookOffsetToBone(upperChestBoneIndex,
                          smoothedSpineYaw * 0.08f,
                          0.0f,
                          0.0f,
                          1.0f);

    // 头部只做小范围补偿，避免“头拧太多”。
    applyLookOffsetToBone(headBoneIndex, smoothedHeadYaw, smoothedHeadPitch, 0.0f, 1.0f);

    // 眼球骨骼是可选项：不存在时自动忽略。
    applyLookOffsetToBone(leftEyeBoneIndex, smoothedEyeYaw, smoothedEyePitch, 0.0f, 1.0f);
    applyLookOffsetToBone(rightEyeBoneIndex, smoothedEyeYaw, smoothedEyePitch, 0.0f, 1.0f);
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
            changeState(trans.toState, trans.blendDuration);
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

    // 若时间早于第一个关键点，直接取最前
    if (time <= keys.front().time) {
        const auto& k = keys.front();
        return QVector3D(k.x, k.y, k.z);
    }

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

    if (time <= keys.front().time) {
        const auto& k = keys.front();
        return QQuaternion(k.w, k.x, k.y, k.z);
    }

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

void AnimationPlayer::changeState(const std::string& targetState, double transitionDuration){
    if (currentStateName == targetState) return;
    if (!myStateMachine) return;

    auto it = myStateMachine->stateIndexMap.find(targetState);
    if (it == myStateMachine->stateIndexMap.end()) {
        qDebug() << "[AnimationPlayer] State " << targetState.c_str() << " not found";
        return;
    }

    int stateIndex = it->second;
    const AnimationState& target = myStateMachine->states[stateIndex];

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

    if (transitionDuration > 0.0 && currentClip != nullptr) {
        m_crossfader.startFade(poseFinal, transitionDuration);
    }
    
    qDebug() << "[AnimationPlayer] changeState from" << currentStateName.c_str() << "to" << targetState.c_str() << "blendDuration=" << transitionDuration;

    currentStateName = targetState;
    currentClip = &clipIt->second;
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