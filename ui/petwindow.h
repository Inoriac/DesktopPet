//
// Created by Inoriac on 2025/10/15.
//

#ifndef DESKTOP_PET_PETWINDOW_H
#define DESKTOP_PET_PETWINDOW_H
#include <QWidget>

class RenderViewport;

class PetWindow : public QWidget{
    Q_OBJECT

public:
    explicit PetWindow(const QString modelName, QWidget *parent = nullptr);
    ~PetWindow();

    void applySettings(int sizePercent, bool alwaysOnTop, bool clickThrough);

signals:
    void aboutToClose();  // 窗口即将关闭时发送

protected:
    // 鼠标事件处理
    // 拖拽
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

    // 窗口事件
    void closeEvent(QCloseEvent *event) override;

private:
    void setupWindow();
    void setupRenderViewport();
    void updateWindowFlags(bool alwaysOnTop, bool clickThrough);

    // 拖拽相关
    bool isDragging;
    QPoint dragStartPosition;

    // 渲染组件
    RenderViewport *renderViewport;
    QString modelName;

    // 设置
    int sizePercent;
    bool alwaysOnTop;
    bool clickThrough;
};

#endif //DESKTOP_PET_PETWINDOW_H