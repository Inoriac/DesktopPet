#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QSurfaceFormat>

#include "ai_types.h"
#include "entity/pet.h"
#include "ui/petwindow.h"
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

    // ConfigManager 构造时已自动 load 一次默认路径；此处二次 load 以启动器配置覆盖。
    if (!configPath.isEmpty()) {
        ConfigManager::instance().loadConfig(configPath);
    }

    // 初始化统计系统，启用落盘（默认 log/statistics.json）。
    StatisticManager::getInstance().initialize();

    // —— 直接承载桌宠窗口（原控制面板 MainWindow 已由 Python launcher 取代）——
    // 角色来源：--pet 入参；未指定时退回注册表首个角色；都没有则提示用 launcher 启动。
    // 注意：Pet 注册表不会在 instance() 构造时自动载入 pets.json，必须显式 load()
    // （原由 MainWindow::loadPetList() 触发，移除面板后改由 main 直接调用）。
    Pet::instance().load();
    QString petName = parser.value(petOption);
    if (petName.isEmpty()) {
        const QStringList names = Pet::instance().getPetNames();
        if (!names.isEmpty()) {
            petName = names.first();
        }
    }
    if (petName.isEmpty() || !Pet::instance().hasPet(petName)) {
        qWarning() << "[main] No valid pet to start. Please launch via the Python launcher.";
        return 0;
    }

    const QString modelPath = Pet::instance().getModelPath(petName);
    if (modelPath.isEmpty()) {
        qWarning() << "[main] Cannot find pet model for:" << petName;
        return 0;
    }

    // 配置全由 ConfigManager 提供（launcher 写入 launch_config.json 后经 loadConfig 载入），
    // 不再依赖任何 UI 控件读取——等价于原 MainWindow::OnStartPet 的核心逻辑。
    auto &cfg = ConfigManager::instance();
    const int sizePercent = cfg.getPetScalePercent();
    const bool alwaysOnTop = cfg.isPetAlwaysOnTop();
    const bool clickThrough = cfg.isPetClickThrough();
    const bool aiEnabled = cfg.getLlmConfig().enabled;
    const ScreenChatConfig screenChat = cfg.getScreenChatConfig();
    const VoiceConfig voice = cfg.getVoiceConfig();
    cfg.setLlmEnabled(aiEnabled);
    cfg.setVoiceConfig(voice);

    PetWindow *pet = new PetWindow(petName, nullptr);
    StatisticManager::getInstance().recordPetStart(petName);
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [petName]() {
        StatisticManager::getInstance().recordPetStop(petName);
        StatisticManager::getInstance().saveStatistics();
    });
    pet->applySettings(sizePercent, alwaysOnTop, clickThrough, aiEnabled,
                       screenChat, voice);
    pet->show();

    return app.exec();
}
