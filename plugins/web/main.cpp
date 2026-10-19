#include "web_command.h"
#include <QGuiApplication>
#include <QFile>
#include <cstdio>

int main(int argc, char** argv) {
    // QTextDocument needs GUI font/palette services for markup such as <code>.
    // The helper stays headless and never creates a window.
    QCoreApplication::setAttribute(Qt::AA_PluginApplication);
    QGuiApplication application(argc, argv);
    QFile input, output;
    if (!input.open(stdin, QIODevice::ReadOnly) || !output.open(stdout, QIODevice::WriteOnly)) return 1;
    return runWebCommand(application.arguments().mid(1), input, output);
}
