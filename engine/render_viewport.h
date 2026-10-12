//
// Created by Inoriac on 2025/10/16.
//

#ifndef DESKTOP_PET_RENDER_VIEWPORT_H
#define DESKTOP_PET_RENDER_VIEWPORT_H

#include <QOpenGLWidget>
#include <memory>
#include <QElapsedTimer>
#include <QTimer>
#include "../ui/avatar_view.h"
#include "presentation/avatar_presentation.h"

#include "shader_manager.h"
#include "animation/animation_manager.h"

class RenderEngine;
class AnimationManager;
class Avatar3DBackend;

class RenderViewport : public QOpenGLWidget, public AvatarView{
    Q_OBJECT
public:
    explicit RenderViewport(QWidget *parent = nullptr);
    ~RenderViewport() override;

    RenderEngine* getRenderEngine() const { return renderEngine.get(); }
    AnimationManager* getAnimationManager() const { return animationManager.get(); }

    void bindPresentation(AvatarPresentation* presentation);
    void setRenderingEnabled(bool enabled);
    bool renderingEnabled() const { return m_renderingEnabled; }
    QString hitRegion(const QPoint& localPosition) const override;
    bool handleCameraKey(int key, Qt::KeyboardModifiers modifiers) override;
    // 动态加载模型和动画
    bool loadModel(const QString& modelPath);
    void clearModel();
    
    // 获取初始化状�?
    bool isInitialized() const { return renderEngine && animationManager; }
    
signals:
    // 初始化完成信�?
    void initializationCompleted();

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private:
    void attachBackend();
    QPointer<AvatarPresentation> m_presentation;
    std::unique_ptr<Avatar3DBackend> m_avatarBackend;
    QTimer m_frameTimer;
    QElapsedTimer m_frameClock;
    bool m_renderingEnabled = false;
    bool m_modelSelected = false;
    std::unique_ptr<QOpenGLFunctions_3_3_Core> glCore;
    std::unique_ptr<RenderEngine> renderEngine;     // 渲染引擎
     std::unique_ptr<ShaderManager> shaderManager;   // 着色器
     std::unique_ptr<AnimationManager> animationManager;  // 动画管理�?
     QString currentModelPath;  // 当前加载的模型路�?
     QString currentAnimationPath;  // 当前加载的动画路�?

     // 初始化动画管理器
     void initializeAnimationManager();
 };

#endif //DESKTOP_PET_RENDER_VIEWPORT_H
