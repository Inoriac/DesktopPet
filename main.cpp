#include <QApplication>
#include <qdir.h>
#include <QSurfaceFormat>

#include "ui/mainwindow.h"

int main(int argc, char *argv[])
{
    // qputenv("QT_DEBUG_PLUGINS", "1");  // 临时打开插件日志

    // 要求 OpenGL 3.3 Core，上下文需在 QApplication 创建前设置
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setDepthBufferSize(24);
    fmt.setStencilBufferSize(8);
    fmt.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    QSurfaceFormat::setDefaultFormat(fmt);

    QApplication app(argc, argv);
    app.setApplicationName("Desktop Pet");
    app.setOrganizationName("Desktop Pet Team");
    app.setApplicationVersion("1.0.0");

    QDir::setCurrent(QCoreApplication::applicationDirPath() + "/..");

    MainWindow w;
    w.show();

    return app.exec();
}