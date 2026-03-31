#include "animation_crossfader.h"
#include <cmath>

void AnimationCrossfader::startFade(const AnimationPose& currentPose, double duration) {
    if (duration <= 0.0) {
        m_isFading = false;
        return;
    }

    m_snapshotPose = currentPose;

    m_fadeDuration = duration;
    m_currentFadeTime = 0.0;
    m_blendWeight = 0.0;
    m_isFading = true;
}

void AnimationCrossfader::update(double deltaTime) {
    if (!m_isFading) return;

    m_currentFadeTime += deltaTime;

    if (m_currentFadeTime >= m_fadeDuration) {
        m_isFading = false;
        m_blendWeight = 1.0;
        return;
    }

    m_blendWeight = m_currentFadeTime / m_fadeDuration;
    m_blendWeight = std::clamp(m_blendWeight, 0.0, 1.0);
}

void AnimationCrossfader::blendPoses(const AnimationPose& source, const AnimationPose& target, AnimationPose& out, double weight) {
    if (source.bonePoses.size() != target.bonePoses.size()) {
        out = target;
        return;
    }
    
    size_t boneCount = target.bonePoses.size();
    if (out.bonePoses.size() != boneCount) {
        out.bonePoses.resize(boneCount);
    }
    
    for (size_t i = 0; i < boneCount; ++i) {
        const BonePose& a = source.bonePoses[i];
        const BonePose& b = target.bonePoses[i];
        BonePose& res = out.bonePoses[i];
        
        res.translation = a.translation * (1.0 - weight) + b.translation * weight;
        res.scale = a.scale * (1.0 - weight) + b.scale * weight;
        res.rotation = QQuaternion::slerp(a.rotation, b.rotation, weight);
    }
}
