//
// Created by Inoriac on 2025/11/20.
//

#include "animation_player.h"

#include <random>

AnimationPlayer::AnimationPlayer(const Skeleton *skeleton, const std::unordered_map<std::string, AnimationClip> *clips,
                                 const AnimationStateMachineDefinition *stateMachine)
    : mySkeleton(skeleton), myClips(clips), myStateMachine(stateMachine){
    size_t boneCount = mySkeleton ? mySkeleton->bones.size() : 0;

    poseCurrent = AnimationPose(boneCount);
    posePrevious = AnimationPose(boneCount);
    poseFinal = AnimationPose(boneCount);

    // 默认状态
    currentStateName = myStateMachine->defaultState;
    currentTime = 0.0;

    const AnimationState& st = myStateMachine->states[myStateMachine->stateIndexMap.at(currentStateName)];
    selectRandomClipForState(st);
}

void AnimationPlayer::selectRandomClipForState(const AnimationState& state) {
    // TODO: 此处尚未引入权重的概念，未对其进行处理，后续可以考虑加入
    const auto& it = state.clipOptions;
    if (it.empty()) {
        currentClipIndex = -1;
        return;
    }

    std::uniform_int_distribution<int> u(0, it.size() - 1);
    currentClipIndex = u(rng);
}

void AnimationPlayer::update(double deltaTime) {

}

void AnimationPlayer::triggerEvent(const std::string &eventName) {
}

void AnimationPlayer::sampleClip(const AnimationClip& clip, double time, AnimationPose& outPose) {

}

QVector3D AnimationPlayer::sampleVec3(const std::vector<KeyFrameVec3>& keys, double time){

}

QQuaternion AnimationPlayer::sampleQuat(const std::vector<KeyFrameQuat>& keys, double time){

}

void AnimationPlayer::updateFinalPose(){

}

void AnimationPlayer::changeState(const std::string& targetState){

}
