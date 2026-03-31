#ifndef ANIMATION_CROSSFADER_H
#define ANIMATION_CROSSFADER_H

#include "animation_types.h"
#include <QQuaternion>
#include <QVector3D>
#include <algorithm>



class AnimationCrossfader {
public:
    AnimationCrossfader() = default;

    // 开始一个新的跨状态平滑混合
    void startFade(const AnimationPose& currentPose, double duration);
    
    // 更新混合内部时钟，并返回当前的插值权重 (0.0 到 1.0)
    void update(double deltaTime);
    
    bool isFading() const { return m_isFading; }
    double getBlendWeight() const { return m_blendWeight; }
    
    // 获取需要继续采样的旧动画状态
    const AnimationPose& getSnapshot() const { return m_snapshotPose; }

    // 核心混合器：执行球面线性插值(旋转)和线性插值(平移/缩放)
    static void blendPoses(const AnimationPose& source, const AnimationPose& target, AnimationPose& out, double weight);

private:
    AnimationPose m_snapshotPose;
    double m_fadeDuration = 0.0;
    double m_currentFadeTime = 0.0;
    double m_blendWeight = 0.0;
    bool m_isFading = false;
};

#endif // ANIMATION_CROSSFADER_H
