#pragma once
#include <QStandardPaths>
#include <QString>

inline QString testPythonExecutable() {
    const auto configured = qEnvironmentVariable("DESKTOP_PET_TEST_PYTHON");
    if (!configured.isEmpty()) return configured;
#ifdef Q_OS_WIN
    return QStandardPaths::findExecutable("python");
#else
    return QStandardPaths::findExecutable("python3");
#endif
}
