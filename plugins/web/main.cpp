#include "web_command.h"
#include <QCoreApplication>
#include <QFile>
#include <cstdio>

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QFile input, output;
    if (!input.open(stdin, QIODevice::ReadOnly) || !output.open(stdout, QIODevice::WriteOnly)) return 1;
    return runWebCommand(application.arguments().mid(1), input, output);
}
