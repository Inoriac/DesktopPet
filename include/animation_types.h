#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <QVector3D>
#include <QQuaternion>
#include <QMatrix4x4>

// 基本关键帧结构
struct KeyFrameVec3 {
    double time;
    float x, y, z;
};

struct KeyFrameQuat {
    double time;
    float x, y, z, w;  // quaternion
};

// 动画通道
struct AnimationChannel
{
    int boneIndex = -1;

    std::vector<KeyFrameVec3> translationKeys;
    std::vector<KeyFrameQuat> rotationKeys;
    std::vector<KeyFrameVec3> scaleKeys;

    bool hasTranslation() const { return !translationKeys.empty(); }
    bool hasRotation() const { return !rotationKeys.empty(); }
    bool hasScale() const { return !scaleKeys.empty(); }
};

// 一个完整动作
struct AnimationClip {
    std::string name;           // 动画名称
    double duration = 0.0;      // 动画总时长(秒)
    double fps = 30.0;          // TODO: 供采样用，应该是全局设置中的一个参数

    // boneIndex -< channel
    std::vector<AnimationChannel> channels;

    AnimationChannel* findChannel(int boneIndex) {
        for (auto& ch : channels) {
            if (ch.boneIndex == boneIndex) return &ch;
        }
        return nullptr;
    }
};

// 状态机
// 候选动画
struct AnimationCLipOption {
    std::string clipName;   // 动画文件名
    float weight = 1.0f;
};

// 单个状态定义
struct AnimationState {
    std::string name;       // 状态名称
    std::vector<AnimationCLipOption> clipOptions;   // 候选动画列表
    bool loop = true;       // 是否循环播放
};

// 状态转换定义
struct AnimationTransition
{
    std::string fromState;      // 起始状态
    std::string toState;        // 目标状态
    std::string condition;      // 事件名称
    double blendDuration = 0.2; // 过渡时间
};

// 状态机完整定义,由 JSON 加载
struct AnimationStateMachineDefinition
{
    std::string defaultState;

    std::vector<AnimationState> states;
    std::vector<AnimationTransition> transactions;

    // 提升查询效率用
    std::unordered_map<std::string, int> stateIndexMap;
    std::unordered_map<std::string, std::vector<int>> transactionMap;   // 记录每个状态可达的目标状态index

    void buildLookupTables() {
        stateIndexMap.clear();
        transactionMap.clear();

        for (int i = 0; i < states.size(); i++) {
            stateIndexMap[states[i].name] = i;
        }

        for (int i = 0; i < transactions.size(); i++) {
            transactionMap[transactions[i].fromState].push_back(i);
        }
    }
};

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
