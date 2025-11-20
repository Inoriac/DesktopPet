//
// Created by Inoriac on 2025/11/18.
//

#include <iostream>

#include "animation_importer.h"

AnimationClip AnimationImporter::loadAnimation(const std::string &filepath,
    const std::unordered_map<std::string, int> &boneNameToIndexMap) {
    tinygltf::Model model;

    if (!loadGltfFile(filepath, model)) {
        return AnimationClip{};
    }

    if (model.animations.empty()) {
        std::cerr << "[AnimationImporter] No animation found in: " << filepath << std::endl;
        return AnimationClip{};
    }

    const tinygltf::Animation& animation = model.animations[0];

    // 文件名为 clip 名
    std::string clipName = filepath.substr(filepath.find_last_of("/\\") + 1);
    size_t dot = clipName.find('.');
    if (dot != std::string::npos) clipName = clipName.substr(0, dot);

    return extractAnimationClip(model, animation, boneNameToIndexMap, clipName);
}

bool AnimationImporter::loadGltfFile(const std::string &filepath, tinygltf::Model &model) {
    tinygltf::TinyGLTF loader;
    std::string err, warn;

    bool ret = loader.LoadASCIIFromFile(&model, &warn, &err, filepath);

    if (!warn.empty()) {
        std::cerr << "[AnimationImporter] Warn: " << warn << std::endl;
    }

    if (!err.empty()) {
        std::cerr << "[AnimationImporter] Error: " << err << std::endl;
    }

    if (!ret) {
        std::cerr << "[AnimationImporter] Failed to load glTF file: " << filepath << std::endl;
        return false;
    }

    return true;
}

AnimationClip AnimationImporter::extractAnimationClip(const tinygltf::Model &model, const tinygltf::Animation &anim,
    const std::unordered_map<std::string, int> &boneNameToIndexMap, const std::string &clipName) {
    AnimationClip clip;
    clip.name = clipName;
    clip.duration = 0.0;

    for (const auto& channel : anim.channels) {
        if (channel.sampler < 0 || channel.sampler >= static_cast<int>(anim.samplers.size())) {
            std::cerr << "[AnimationImporter] Invaild sampler index in channel" << std::endl;
            continue;
        }
        const tinygltf::AnimationSampler& sampler = anim.samplers[channel.sampler];
        extractChannelToClip(clip, model, channel, sampler, boneNameToIndexMap);

        // 记录持续时间
        if (sampler.input >= 0 && sampler.input <= static_cast<int>(model.accessors.size())) {
            const tinygltf::Accessor& acc = model.accessors[sampler.input];
            if(!acc.minValues.empty() || !acc.maxValues.empty()) {
                if(!acc.maxValues.empty()) {
                    double maxT = acc.maxValues[0];
                    clip.duration = std::max(clip.duration, maxT);
                } else if ( !acc.minValues.empty()) {
                    double minT = acc.minValues[0];
                    clip.duration = std::max(clip.duration, minT);
                }
            } else {
                // as fallback, we will scan the times during extracting sampler
                // (the extractSampler* will still fill times; we'll compute duration there if needed)
            }
        }
    }

    // 另一种保险做法：遍历 channels 的 time arrays 寻找 max
    double computedMax = 0.0;
    for (const auto& ch : clip.channels) {
        for (const auto& k : ch.translationKeys) computedMax = std::max(computedMax, k.time);
        for (const auto& k : ch.rotationKeys)    computedMax = std::max(computedMax, k.time);
        for (const auto& k : ch.scaleKeys)       computedMax = std::max(computedMax, k.time);
    }
    clip.duration = std::max(clip.duration, computedMax);

    return clip;
}

void AnimationImporter::extractChannelToClip(AnimationClip &clip, const tinygltf::Model &model,
    const tinygltf::AnimationChannel &channel, const tinygltf::AnimationSampler &sampler,
    const std::unordered_map<std::string, int> &boneNameToIndexMap) {
    if (channel.target_node < 0 || channel.target_node >= model.nodes.size()) {
        std::cerr << "[AnimationImporter] channel.target_node out of range" << std::endl;
        return;
    }

    const tinygltf::Node &node = model.nodes[channel.target_node];
    const std::string& nodeName = node.name;

    if (nodeName.empty()) {
        std::cerr << "[AnimationImporter] channel target node has no name, skipping" << std::endl;
        return;
    }

    auto it = boneNameToIndexMap.find(nodeName);
    if (it == boneNameToIndexMap.end()) {
        std::cerr << "[AnimationImporter] Node not found in skeleton:" << nodeName << std::endl;
        return;
    }

    int boneIndex = it->second;

    // 确保该骨骼有 AnimationChannel
    AnimationChannel* animChannel = clip.findChannel(boneIndex);
    if (!animChannel) {
        AnimationChannel newCh;
        newCh.boneIndex = boneIndex;
        clip.channels.push_back(std::move(newCh));
        animChannel = clip.findChannel(boneIndex);
        if (!animChannel) {
            std::cerr << "[AnimationImporter] failed to create channel for boneIndex " << boneIndex << "\n";
            return;
        }
    }

    const std::string& path = channel.target_path;

    // translation / rotation / scale
    if (path == "translation") {
        std::vector<float> times;
        std::vector<KeyFrameVec3> frames;
        extractSamplerVec3(model, sampler, times, frames);
        animChannel->translationKeys.insert(animChannel->translationKeys.end(), frames.begin(), frames.end());
    } else if (path == "rotation") {
        std::vector<float> times;
        std::vector<KeyFrameQuat> frames;
        extractSamplerQuat(model, sampler, times, frames);
        animChannel->rotationKeys.insert(animChannel->rotationKeys.end(), frames.begin(), frames.end());
    } else if (path == "scale") {
        std::vector<float> times;
        std::vector<KeyFrameVec3> frames;
        extractSamplerVec3(model, sampler, times, frames);
        animChannel->scaleKeys.insert(animChannel->scaleKeys.end(), frames.begin(), frames.end());
    } else {
        std::cerr << "[AnimationImporter] Unsupported channel path:" << path << std::endl;
    }
}

void AnimationImporter::extractSamplerVec3(const tinygltf::Model &model, const tinygltf::AnimationSampler &sampler,
    std::vector<float> &outTimes, std::vector<KeyFrameVec3> &outKeyframes) {
    outTimes.clear();
    outKeyframes.clear();

    // 读取 times
    std::vector<float> flatTimes;
    readAccessorData(model, sampler.input, flatTimes);
    outTimes = flatTimes;

    // 读取 outputs
    std::vector<float> flatValues;
    readAccessorData(model, sampler.output, flatValues);
    if(flatValues.empty()) {
        std::cerr << "[AnimationImporter] extractSamplerVec3: output values size invalid" << std::endl;
        return;
    }

    int numComponents = 3;
    size_t count = flatValues.size() / numComponents;
    outKeyframes.resize(count);
    for (size_t i = 0; i < count; i++) {
        KeyFrameVec3 k;
        k.time = (i < outTimes.size()) ? outTimes[i] : 0.0f;
        k.x = flatValues[i * numComponents + 0];
        k.y = flatValues[i * numComponents + 1];
        k.z = flatValues[i * numComponents + 2];
        outKeyframes[i] = k;
    }
}

void AnimationImporter::extractSamplerQuat(const tinygltf::Model &model, const tinygltf::AnimationSampler &sampler,
    std::vector<float> &outTimes, std::vector<KeyFrameQuat> &outKeyframes) {
    outTimes.clear();
    outKeyframes.clear();

    // 读取 times
    std::vector<float> flatTimes;
    readAccessorData(model, sampler.input, flatTimes);
    outTimes = flatTimes;

    // 读取 outputs
    std::vector<float> flatValues;
    readAccessorData(model, sampler.output, flatValues);
    if(flatValues.empty()) {
        std::cerr << "[AnimationImporter] extractSamplerQuat: output values size invalid" << std::endl;
        return;
    }

    int numComponents = 4;
    size_t count = flatValues.size() / numComponents;
    outKeyframes.resize(count);
    for (size_t i = 0; i < count; i++) {
        KeyFrameQuat k;
        k.time = (i < outTimes.size()) ? outTimes[i] : 0.0f;
        k.x = flatValues[i * numComponents + 0];
        k.y = flatValues[i * numComponents + 1];
        k.z = flatValues[i * numComponents + 2];
        k.w = flatValues[i * numComponents + 3];
        outKeyframes[i] = k;
    }
}

template<typename T>
void AnimationImporter::readAccessorData(const tinygltf::Model &model, int accessorIndex, std::vector<T> &output) {
    output.clear();
    
    // if(accessorIndex < 0 || accessorIndex >= static_cast<int>(model.accessors.size())) {
    //     std::cerr << "[AnimationImporter] accessorIndex out of range\n";
    //     return;
    // }

    const tinygltf::Accessor& accessor = model.accessors[accessorIndex];
    const tinygltf::BufferView& bufferView = model.bufferViews[accessor.bufferView];
    const tinygltf::Buffer& buffer = model.buffers[bufferView.buffer];

    const unsigned char* dataPtr = &buffer.data[bufferView.byteOffset + accessor.byteOffset];
    size_t count = accessor.count;
    size_t componentSize = sizeof(float);

    int numComponents = 1;
    switch (accessor.type) {
        case TINYGLTF_TYPE_SCALAR: numComponents = 1; break;
        case TINYGLTF_TYPE_VEC2:   numComponents = 2; break;
        case TINYGLTF_TYPE_VEC3:   numComponents = 3; break;
        case TINYGLTF_TYPE_VEC4:   numComponents = 4; break;
        default:
            std::cerr << "[AnimationImporter] Unsupported accessor type for readAccessorData" << std::endl;
            return;
    }

    size_t stride = bufferView.byteStride ? bufferView.byteStride : (componentSize * numComponents);

    output.resize(count * numComponents);   // 扁平化输出
    for (size_t i = 0; i < count; i++) {
        const unsigned char* src = dataPtr + i * stride;
        std::memcpy(&output[i * numComponents], src, componentSize * numComponents);
    }
}
