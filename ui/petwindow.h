//
// Created by Inoriac on 2025/10/15.
//

#ifndef DESKTOP_PET_PETWINDOW_H
#define DESKTOP_PET_PETWINDOW_H

#include <QMenu>
#include <QAction>
#include <QElapsedTimer>

class RenderViewport;

class PetWindow : public QWidget{
    Q_OBJECT

public:
    explicit PetWindow(const QString modelName, QWidget *parent = nullptr);
    ~PetWindow();

    void applySettings(int sizePercent, bool alwaysOnTop, bool clickThrough);
    bool loadModel(const QString &modelPath);
    
    // 动画控制方法
    void playAnimation();
    void pauseAnimation();
    void stopAnimation();
    void setAnimationSpeed(float speed);
    void setAnimationLoop(bool loop);

signals:
    void aboutToClose();  // 窗口即将关闭时发送
    void requestStop();

protected:
    // 鼠标事件处理
    // 拖拽
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

    // 右键菜单
    void contextMenuEvent(QContextMenuEvent *event) override;

    // 窗口事件
    void closeEvent(QCloseEvent *event) override;

    // 键盘事件(用于调试相机视角)
    void keyPressEvent(QKeyEvent *event) override;

private:
    void setupWindow();
    void setupRenderViewport();
    void setupContextMenu();
    void updateWindowFlags(bool alwaysOnTop, bool clickThrough);

    void unloadModel();

    // 辅助交互方法
    bool canTriggerTouch() const;   // 是否能够触发触摸反应，交互锁
    void triggerTouchReaction(const std::string& tag);  // 触发触摸反应逻辑

    // 交互相关
    bool isDragging = false;
    bool isPressingModel = false;
    std::string hitPartTag;

    QPoint dragStartPosition;
    QPoint pressLocalPosition;

    QElapsedTimer pressTimer;

    // 右键菜单
    QMenu *contextMenu;
    QAction *closeAction;

    // 渲染组件
    RenderViewport *renderViewport;
    QString modelName;

    // 设置
    int sizePercent;
    bool alwaysOnTop;
    bool clickThrough;
};

#endif //DESKTOP_PET_PETWINDOW_H