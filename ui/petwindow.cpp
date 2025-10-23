//
// Created by Inoriac on 2025/10/15.
//

#include "petwindow.h"
#include "render_viewport.h"
#include "pet.h"

#include <QApplication>
#include <qboxlayout.h>
#include <QScreen>
#include <QDebug>
#include <QCloseEvent>

PetWindow::PetWindow(const QString modelName, QWidget *parent)
    : QWidget(parent)
    , isDragging(false)
    , renderViewport(nullptr)
    , modelName(modelName)
    , sizePercent(100)
    , alwaysOnTop(true)
    , clickThrough(false){
    setupWindow();
    setupRenderViewport();
}

PetWindow::~PetWindow() {
    if (renderViewport) {
        delete renderViewport;
    }
}

void PetWindow::applySettings(int sizePercent, bool alwaysOnTop, bool clickThrough) {
    this->sizePercent = sizePercent;
    this->alwaysOnTop = alwaysOnTop;
    this->clickThrough = clickThrough;

    // 更新窗口标志
    updateWindowFlags(alwaysOnTop, clickThrough);

    // 更新大小
    int baseSize = 400;
    int newSize = baseSize * sizePercent / 100;
    resize(newSize, newSize);

    qDebug() << "PetWindow setting applied - size:" << newSize;
}

void PetWindow::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && !clickThrough) {
        isDragging = true;
        dragStartPosition = event->globalPosition().toPoint();
        event->accept();
    }
}

void PetWindow::mouseMoveEvent(QMouseEvent *event) {
    if (isDragging && !clickThrough) {
        move(event->globalPosition().toPoint() - dragStartPosition);
        event->accept();
    }
}

void PetWindow::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        isDragging = false;
        event->accept();
    }
}

void PetWindow::closeEvent(QCloseEvent *event) {
    qDebug() << "PetWindow closing...";
    emit aboutToClose();  // 可以发送信号给 MainWindow
    event->accept();
}

void PetWindow::setupWindow() {
    // 设置窗口属性
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);

    // 设置窗口标志
    updateWindowFlags(alwaysOnTop, clickThrough);
    resize(400, 400);

    // 设置到右下角
    QScreen *screen = QApplication::primaryScreen();
    QRect screenGeometry = screen->availableGeometry();
    int x = screenGeometry.right() - width() - 20;   // 距离右边20像素
    int y = screenGeometry.bottom() - height() - 20; // 距离底部20像素
    move(x, y);

    qDebug() << "PetWindow create with model:" << Pet::instance().getModelPath(modelName);
}

void PetWindow::setupRenderViewport() {
    renderViewport = new RenderViewport(this);
    renderViewport->setMinimumHeight(400);

    // 设置布局
    auto layout = std::make_unique<QVBoxLayout>(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(renderViewport);

    layout.release();

    // 加载模型（这里需要 RenderViewport 支持动态加载）
    // if (!renderViewport->loadModel(Pet::instance().getModelPath(modelName))) {
    //     qWarning() << "Failed to load model:" << Pet::instance().getModelPath(modelName);
    // }
}

void PetWindow::updateWindowFlags(bool alwaysOnTop, bool clickThrough) {
    Qt::WindowFlags flags = Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint;

    if (clickThrough) {
        flags |= Qt::Tool;
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
    } else {
        setAttribute(Qt::WA_TransparentForMouseEvents, false);
    }

    setWindowFlags(flags);

    // 强制更新窗口
    if (isVisible()) {
        hide();
        show();
    }
}
