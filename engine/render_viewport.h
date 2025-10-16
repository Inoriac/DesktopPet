//
// Created by Inoriac on 2025/10/16.
//

#ifndef DESKTOP_PET_RENDER_VIEWPORT_H
#define DESKTOP_PET_RENDER_VIEWPORT_H

#include <QOpenGLWidget>
#include <QOpenGLFunctions_3_3_Core>
#include <memory>

class RenderEngine;

class RenderViewport : public QOpenGLWidget{
    Q_OBJECT
public:
    explicit RenderViewport(QWidget *parent = nullptr);
    ~RenderViewport();

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private:
    std::unique_ptr<QOpenGLFunctions_3_3_Core> glCore;
    std::unique_ptr<RenderEngine> renderEngine;
};

#endif //DESKTOP_PET_RENDER_VIEWPORT_H