//
// Created by Inoriac on 2025/10/15.
//

#ifndef DESKTOP_PET_PETWINDOW_H
#define DESKTOP_PET_PETWINDOW_H

#include <QMenu>
#include <QAction>
#include <QElapsedTimer>
#include <QTimer>
#include <QRect>
#include <memory>

#include "ai/ai_brain.h"
#include "ai/tool_registry.h"

class RenderViewport;

class PetWindow : public QWidget{
    Q_OBJECT

public:
    explicit PetWindow(const QString modelName, QWidget *parent = nullptr);
    ~PetWindow();

    void applySettings(int sizePercent, bool alwaysOnTop, bool clickThrough, bool aiEnabled = false);
    bool loadModel(const QString &modelPath);
    
    // 动画控制方法
    void playAnimation();
    void pauseAnimation();
    void stopAnimation();
    void setAnimationSpeed(float speed);
    void setAnimationLoop(bool loop);
    void setBigScreenAlarm(bool on);

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
    void setupAiBrain();

    void unloadModel();

    // 窗口吸附
    void setupWindowSnapping();
    void updateCachedWindows();
    void updateSnapZone();
    bool trySnap();
    bool isStillNearSnappedWindow() const;
    void followSnappedWindowWhileDragging();
    void followSnappedWindow();
    void exitWindowSnapping(bool triggerWindowStand = true);
    bool isWindowMaximized(void* hwnd) const;
    bool isWindowFullscreen(const QRect& rect) const;
    void refreshTopMostByState();
    void applyTopMostRuntime(bool topMost);
    QRect getNativeWindowRect() const;
    QRect getMovementBounds() const;
    void moveNativeWindow(int x, int y);
    void setupDropAnimation();
    void startDropAnimation(int targetX);
    void stopDropAnimation();
    void beginDragAnimation();
    bool isDraggingAnyWindowSitState() const;

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
    bool aiEnabled = false;

    std::unique_ptr<AIBrain> aiBrain;
    std::unique_ptr<ToolRegistry> aiToolRegistry;

    struct NativeWindowEntry {
        void* hwnd = nullptr;
        QRect rect;
    };

    std::vector<NativeWindowEntry> cachedWindows;
    QTimer* snapFollowTimer = nullptr;
    QTimer* snapScanTimer = nullptr;
    QTimer* dropTimer = nullptr;

    QRect pinkZoneDesktopRect;
    void* snappedWindow = nullptr;
    float snapFraction = 0.5f;
    QPoint lastDesktopPosition;
    int snapCursorY = 0;
    bool wasDragging = false;
    bool wasWindowSitting = false;
    bool pendingSnapOnRelease = false;
    void* pendingSnapTarget = nullptr;
    float pendingSnapFraction = 0.5f;
    bool isDropping = false;
    float dropPosY = 0.0f;
    float dropVelocity = 0.0f;
    float dropAccel = 0.0f;
    int dropTargetY = 0;
    int dropFixedX = 0;
    qint64 dropLastTickMs = 0;

    int snapThreshold = 30;
    int snapVerticalOffset = 0;
    QPoint snapZoneOffset {0, -5};
    QSize snapZoneSize {100, 10};
    int snapFollowIntervalMs = 16;
    int totalWindowSitAnimations = 0;

    bool forceExitOnBigScreenAlarm = true;
    bool isBigScreenAlarm = false;
    QAction* toggleBigScreenAlarmAction = nullptr;
};

#endif //DESKTOP_PET_PETWINDOW_H