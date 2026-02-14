//
// Created by Inoriac on 2025/10/16.
//

#ifndef DESKTOP_PET_RENDER_VIEWPORT_H
#define DESKTOP_PET_RENDER_VIEWPORT_H

#include <QOpenGLWidget>
#include <memory>

#include "shader_manager.h"
#include "animation/animation_manager.h"

class RenderEngine;
class AnimationManager;

class RenderViewport : public QOpenGLWidget{
    Q_OBJECT
public:
    explicit RenderViewport(QWidget *parent = nullptr);
    ~RenderViewport() override;

    RenderEngine* getRenderEngine() const { return renderEngine.get(); }

    // 动态加载模型和动画
    bool loadModel(const QString& modelPath);
    void clearModel();
    
    // 获取初始化状态
    bool isInitialized() const { return renderEngine && animationManager; }
    
signals:
    // 初始化完成信号
    void initializationCompleted();

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private:
    std::unique_ptr<QOpenGLFunctions_3_3_Core> glCore;
    std::unique_ptr<RenderEngine> renderEngine;     // 渲染引擎
     std::unique_ptr<ShaderManager> shaderManager;   // 着色器
     std::unique_ptr<AnimationManager> animationManager;  // 动画管理器
     QString currentModelPath;  // 当前加载的模型路径
     QString currentAnimationPath;  // 当前加载的动画路径

     // 初始化动画管理器
     void initializeAnimationManager();
 };

#endif //DESKTOP_PET_RENDER_VIEWPORT_H