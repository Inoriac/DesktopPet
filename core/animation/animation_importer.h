//
// Created by Inoriac on 2025/11/18.
//

#ifndef ANIMATION_IMPORTER_H
#define ANIMATION_IMPORTER_H

#pragma once

#include <string>
#include <unordered_map>
#include <memory>

#include <tiny_gltf.h>
#include "animation_types.h"

class AnimationImporter {
public:
    AnimationImporter() = default;

    // 加载状态机
    void loadStateMachine(const std::string& filepath, AnimationStateMachineDefinition& outStateMachine);
    // 加载动画文件
    AnimationClip loadAnimation(const std::string& filepath, const std::unordered_map<std::string, int>& boneNameToIndexMap);

private:
    // gltf 解析入口
    bool loadGltfFile(const std::string& filepath, tinygltf::Model& model);

    // 从 tinygltf::Animation 中构造 AnimationClip
    AnimationClip extractAnimationClip(
        const tinygltf::Model& model,
        const tinygltf::Animation& anim,
        const std::unordered_map<std::string, int>& boneNameToIndexMap,
        const std::string& clipName);

    // 解析 sampler (输入=keyTimes, 输出=values)
    void extractSamplerVec3(
            const tinygltf::Model& model,
            const tinygltf::AnimationSampler& sampler,
            std::vector<float>& outTimes,
            std::vector<KeyFrameVec3>& outKeyframes);

    void extractSamplerQuat(
        const tinygltf::Model& model,
        const tinygltf::AnimationSampler& sampler,
        std::vector<float>& outTimes,
        std::vector<KeyFrameQuat>& outKeyframes);

    // 解析 channel (node + path -> TRS)
    void extractChannelToClip(
        AnimationClip& clip,
        const tinygltf::Model& model,
        const tinygltf::AnimationChannel& channel,
        const tinygltf::AnimationSampler& sampler,
        const std::unordered_map<std::string, int>& boneNameToIndexMap);

    // 辅助函数：读取 buffer accessor 数据
    template<typename T>
    void readAccessorData(
        const tinygltf::Model& model,
        int accessorIndex,
        std::vector<T>& output);
};


#endif //ANIMATION_IMPORTER_H