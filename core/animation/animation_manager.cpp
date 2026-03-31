//
// Created by Inoriac on 2025/10/15.
//

#include "animation_manager.h"
#include "animation_importer.h"
#include "model_loader.h"
#include <QDebug>
#include <QDirIterator>
#include <algorithm>

#include "configLoader/config_manager.h"

AnimationManager::AnimationManager() {
    // initialize();
}

AnimationManager::~AnimationManager() {
    clear();
}

void AnimationManager::initialize() {
    importer = std::make_unique<AnimationImporter>();
    importer->loadStateMachine(ConfigManager::instance().getStateMachinePath().toStdString(), stateMachine);
}

bool AnimationManager::loadAnimations(const std::string& stateName, const Skeleton& skeleton) {
    // 找到指定状态
    auto stateIt = stateMachine.stateIndexMap.find(stateName);
    if (stateIt == stateMachine.stateIndexMap.end()) {
        qWarning() << "State" << stateName.c_str() << "not found in state machine.";
        return false;
    }
    AnimationState& targetState = stateMachine.states[stateIt->second];

    std::string targetFolder = targetState.folder.empty() ? targetState.name : targetState.folder;
    std::string animationsPath = ConfigManager::instance().getAnimationsBasePath().toStdString() + targetFolder;
    QDir animDir(QString::fromStdString(animationsPath));
    if (!animDir.exists()) {
        qWarning() << "Animation directory not found:" << animationsPath.c_str();
        return false;
    }

    // 获取所有动画文件，包括子文件夹
    QStringList nameFilters;
    nameFilters << "*.gltf" << "*.glb";

    std::vector<std::string> matchedClipNames;
    QDirIterator it(animDir.path(), nameFilters, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QString filePath = it.next();
        QString fileName = QFileInfo(filePath).baseName();

        // 匹配检查
        if (!targetState.clipMatch.empty()) {
            QStringList keywords = QString::fromStdString(targetState.clipMatch).split("|");
            bool matched = false;
            for (const QString& kw : keywords) {
                if (fileName.contains(kw.trimmed(), Qt::CaseInsensitive)) {
                    matched = true;
                    break;
                }
            }
            if (!matched) continue;
        }

        std::string clipName = fileName.toStdString();
        // 只有当 clips 中没有缓存时才进行加载
        if (clips.find(clipName) == clips.end()) {
            AnimationClip clip = importer->loadAnimation(filePath.toStdString(), skeleton.nameToIndex);
            if (!clip.name.empty()) {
                clipName = clip.name; // 更新为加载后的名字 (可能和文件名一致)
                clips[clipName] = std::move(clip);
                matchedClipNames.push_back(clipName);
            }
        } else {
            matchedClipNames.push_back(clipName);
        }
    }

    if (matchedClipNames.empty()) {
        qDebug() << "No matching animations found for state:" << stateName.c_str() << "in folder:" << animDir.path();
        return false;
    }

    for (const auto& clipName : matchedClipNames) { 
        // 检查是否已存在相同的 clipOption
        auto optionIt = std::find_if(targetState.clipOptions.begin(), targetState.clipOptions.end(),
                                   [&](const AnimationCLipOption& opt) {
                                       return opt.clipName == clipName;
                                   });

        if (optionIt == targetState.clipOptions.end()) {
            AnimationCLipOption option;
            option.clipName = clipName;
            option.weight = 1.0f; // 默认权重
            targetState.clipOptions.push_back(std::move(option));
        }
    }

    if (clips.empty()) {
        qWarning() << "Failed to load any animations";
        return false;
    }

    // stateMachine.buildLookupTables();

    // 日志：输出已加载的动画数量与每个状态的候选数
    qDebug() << "AnimationManager::loadAnimations summary:" << "clips=" << clips.size();
    for (const auto& st : stateMachine.states) {
        qDebug() << " state" << QString::fromStdString(st.name) << "clipOptions=" << st.clipOptions.size();
    }

    return true;
}

std::unique_ptr<AnimationPlayer> AnimationManager::createAnimationPlayer(Skeleton skeleton) {
    if (clips.empty()) {
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
    // 清除已加载的动画片段，但保留状态机定义（由配置文件提供）
    clips.clear();
}
