//
// Created by Inoriac on 2025/10/15.
//

#ifndef DESKTOP_PET_ANIMATION_MANAGER_H
#define DESKTOP_PET_ANIMATION_MANAGER_H

#include <string>
#include <unordered_map>
#include <memory>
#include <vector>

#include "animation_player.h"
#include "animation_types.h"

class AnimationImporter;
class Skeleton;

class AnimationManager {
public:
    AnimationManager();
    ~AnimationManager();

    // 初始化动画管理器
    void initialize();
    // 加载动画文件
    bool loadAnimations(const std::string& stateName, const Skeleton& skeleton);
    // 创建动画播放器
    std::unique_ptr<AnimationPlayer> createAnimationPlayer(Skeleton skeleton);
    // 获取动画片段
    const std::unordered_map<std::string, AnimationClip>& getClips() const { return clips; }
    // 获取状态机
    const AnimationStateMachineDefinition& getStateMachine() const { return stateMachine; }
    // 清理资源
    void clear();

private:
    std::unique_ptr<AnimationImporter> importer;
    std::unordered_map<std::string, AnimationClip> clips;
    AnimationStateMachineDefinition stateMachine;

    // 构建动画状态机
    void buildStateMachine();
};

#endif //DESKTOP_PET_ANIMATION_MANAGER_H