#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QSurfaceFormat>

#include "ui/mainwindow.h"
#include "ui/theme_manager.h"
#include "core/configLoader/config_manager.h"
#include "statistic_manager.h"

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
    ThemeManager::instance().applyTo(&app);

    QDir::setCurrent(QCoreApplication::applicationDirPath() + "/..");

    // 命令行参数（供 Python launcher 调用）：
    //   --config <file>  指定启动配置文件（建议绝对路径）
    //   --pet <name>     指定启动角色（须存在于宠物注册表 pets.json）
    // 主题不走命令行，由 ThemeManager 经同名 QSettings(键 ui/theme)读取，见前端修改计划 §3.1。
    QCommandLineParser parser;
    parser.setApplicationDescription("Desktop Pet Application");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption configOption("config",
        "Path to launch configuration file (absolute path recommended)", "config-file");
    QCommandLineOption petOption("pet",
        "Pet name to auto-start (must exist in pets.json registry)", "pet-name");
    parser.addOption(configOption);
    parser.addOption(petOption);
    parser.process(app);

    const QString configPath = parser.value(configOption);
    const QString petName = parser.value(petOption);

    // ConfigManager 构造时已自动 load 一次默认路径；此处二次 load 以启动器配置覆盖。
    if (!configPath.isEmpty()) {
        ConfigManager::instance().loadConfig(configPath);
    }

    // 初始化统计系统，启用落盘（默认 log/statistics.json）。
    StatisticManager::getInstance().initialize();

    MainWindow w(petName);
    w.show();

    // 指定了有效角色则启动后自动开宠（角色无效时 PetWindow 内部静默忽略，回到面板等待）。
    if (!petName.isEmpty()) {
        w.autoStartPet();
    }

    return app.exec();
}