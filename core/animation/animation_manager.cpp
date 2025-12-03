//
// Created by Inoriac on 2025/10/15.
//

#include "animation_manager.h"
#include "animation_importer.h"
#include "model_loader.h"
#include <QDir>
#include <QDebug>

AnimationManager::AnimationManager() {
    initialize();
}

AnimationManager::~AnimationManager() {
    clear();
}

void AnimationManager::initialize() {
    importer = std::make_unique<AnimationImporter>();
}

bool AnimationManager::loadAnimations(const std::string& animationsPath, const Skeleton& skeleton) {
    QDir animDir(QString::fromStdString(animationsPath));
    if (!animDir.exists()) {
        qWarning() << "Animation directory not found:" << animationsPath.c_str();
        return false;
    }

    // 获取所有动画文件
    QStringList nameFilters;
    nameFilters << "*.gltf" << "*.glb";
    QFileInfoList animFiles = animDir.entryInfoList(nameFilters, QDir::Files);

    if (animFiles.isEmpty()) {
        qWarning() << "No animation files found in:" << animationsPath.c_str();
        return false;
    }

    // 加载每个动画文件
    for (const auto& fileInfo : animFiles) {
        QString filePath = fileInfo.filePath();
        AnimationClip clip = importer->loadAnimation(filePath.toStdString(), skeleton.nameToIndex);
        if (!clip.name.empty()) {
            clips[clip.name] = std::move(clip);
        }
    }

    if (clips.empty()) {
        qWarning() << "Failed to load any animations";
        return false;
    }

    // 构建动画状态机
    buildStateMachine();
    return true;
}

std::unique_ptr<AnimationPlayer> AnimationManager::createAnimationPlayer(const Skeleton* skeleton) {
    if (clips.empty() || skeleton == nullptr) {
        return nullptr;
    }

    return std::make_unique<AnimationPlayer>(skeleton, &clips, &stateMachine);
}

void AnimationManager::buildStateMachine() {
    // 构建状态机定义
    // 这里可以根据实际需求实现状态机的构建逻辑
    // 例如，根据动画名称自动创建状态和转换

    // 示例：创建默认状态机
    AnimationState idleState;
    idleState.name = "idle";
    idleState.loop = true;

    // 查找idle相关动画
    for (const auto& [clipName, clip] : clips) {
        if (clipName.find("idle") != std::string::npos) {
            AnimationCLipOption option;
            option.clipName = clipName;
            option.weight = 1.0f;
            idleState.clipOptions.push_back(std::move(option));
        }
    }

    // 同样的方式创建其他状态...

    // 添加状态到状态机
    if (!idleState.clipOptions.empty()) {
        stateMachine.states.push_back(std::move(idleState));
    }

    // 设置初始状态
    if (!stateMachine.states.empty()) {
        stateMachine.defaultState = stateMachine.states[0].name;
    }
}

void AnimationManager::clear() {
    clips.clear();
    stateMachine.states.clear();
    stateMachine.transactions.clear();
}
