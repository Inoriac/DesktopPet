#include <QApplication>
#include <QPushButton>

#include <QApplication>
#include "ui/mainwindow.h"

int main(int argc, char *argv[])
{
    qputenv("QT_DEBUG_PLUGINS", "1");  // 临时打开插件日志
    QApplication app(argc, argv);
    MainWindow w;
    w.show();
    return app.exec();
}