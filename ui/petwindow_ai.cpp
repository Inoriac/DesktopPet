//
// Created by Inoriac on 2025/10/15.
//

#include "petwindow.h"

#include "ai/tools/animation_tools.h"
#include "ai/tools/companion_tools.h"
#include "ai/tools/environment_tools.h"
#include "ai/tools/file_tools.h"
#include "ai/tools/life_tools.h"
#include "ai/tools/music_tools.h"
#include "ai/tools/schedule_tools.h"
#include "ai/tools/web_tools.h"
#include "configLoader/config_manager.h"
#include "render_engine.h"
#include "render_viewport.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QMetaObject>

void PetWindow::setupAiBrain() {
    if (!aiBrain || !renderViewport || !renderViewport->getRenderEngine()) {
        return;
    }

    auto* player = renderViewport->getRenderEngine()->getAnimationPlayer();
    auto* animationManager = renderViewport->getAnimationManager();
    if (!player) {
        qWarning() << "[AIBrain] AnimationPlayer not ready, skip setup";
        return;
    }
    if (!animationManager) {
        qWarning() << "[AIBrain] AnimationManager not ready, skip setup";
        return;
    }

    // 设置文件/命令工具允许的根目录。默认来自配置，未配置时退回到应用目录和当前工作目录。
    const AiToolAccessPolicy& toolAccessPolicy = ConfigManager::instance().getAiToolAccessPolicy();
    m_allowedRoots.clear();
    m_allowedRoots.append(toolAccessPolicy.allowedRoots);
    if (m_allowedRoots.isEmpty()) {
        m_allowedRoots.append(QCoreApplication::applicationDirPath());
        m_allowedRoots.append(QDir::currentPath());
    }

    aiToolRegistry = std::make_unique<ToolRegistry>();
    agentScheduler = std::make_unique<AgentScheduler>(this);
    aiToolRegistry->registerTool(std::make_unique<ShowChatBubbleTool>([this](const QString& text, int durationMs) {
        QMetaObject::invokeMethod(this, [this, text, durationMs]() {
            showBubbleMessage(text, durationMs);
            speakPetReply(text, QStringLiteral("toolBubble"));
        }, Qt::QueuedConnection);
    }));
    aiToolRegistry->registerTool(std::make_unique<NotifyUserTool>([this](const QString& title, const QString& message, int durationMs) {
        const QString bubbleText = title.trimmed().isEmpty()
            ? message
            : QString("%1：%2").arg(title, message);
        QMetaObject::invokeMethod(this, [this, bubbleText, durationMs]() {
            showBubbleMessage(bubbleText, durationMs);
            speakPetReply(bubbleText, QStringLiteral("toolBubble"));
        }, Qt::QueuedConnection);
    }));
    aiToolRegistry->registerTool(std::make_unique<SetProactiveModeTool>([this](const QString& mode, int quietMinutes) {
        Q_UNUSED(quietMinutes)
        const QString text = mode == "focus"
            ? QStringLiteral("好，我会安静一点。")
            : QStringLiteral("主动模式已切换为 %1。").arg(mode);
        QMetaObject::invokeMethod(this, [this, text]() {
            showBubbleMessage(text, 3000);
            speakPetReply(text, QStringLiteral("toolBubble"));
        }, Qt::QueuedConnection);
    }));
    aiToolRegistry->registerTool(std::make_unique<ScheduleCreateTool>(agentScheduler.get()));
    aiToolRegistry->registerTool(std::make_unique<ScheduleListTool>(agentScheduler.get()));
    aiToolRegistry->registerTool(std::make_unique<ScheduleCancelTool>(agentScheduler.get()));
    aiToolRegistry->registerTool(std::make_unique<ScheduleSnoozeTool>(agentScheduler.get()));
    aiToolRegistry->registerTool(std::make_unique<PlayAnimationTool>(player));
    aiToolRegistry->registerTool(std::make_unique<GetCurrentAnimationTool>(player));
    aiToolRegistry->registerTool(std::make_unique<GetIdleTransitionCandidatesTool>(player, animationManager));
    aiToolRegistry->registerTool(std::make_unique<GetActionTransitionStatusTool>(player));
    aiToolRegistry->registerTool(std::make_unique<RequestIdleTransitionTool>(player, animationManager));
    aiToolRegistry->registerTool(std::make_unique<GetCurrentTimeTool>());
    aiToolRegistry->registerTool(std::make_unique<GetUserIdleStateTool>());
    aiToolRegistry->registerTool(std::make_unique<GetBatteryStatusTool>());
    aiToolRegistry->registerTool(std::make_unique<GetNetworkStatusTool>());
    aiToolRegistry->registerTool(std::make_unique<LxMusicStatusTool>());
    aiToolRegistry->registerTool(std::make_unique<LxMusicPlayTool>());
    aiToolRegistry->registerTool(std::make_unique<LxMusicPauseTool>());
    aiToolRegistry->registerTool(std::make_unique<LxMusicSkipNextTool>());
    aiToolRegistry->registerTool(std::make_unique<LxMusicSkipPrevTool>());
    aiToolRegistry->registerTool(std::make_unique<LxMusicLyricTool>());
    aiToolRegistry->registerTool(std::make_unique<LxMusicVolumeTool>());
    aiToolRegistry->registerTool(std::make_unique<LxMusicSearchPlayTool>());
    aiToolRegistry->registerTool(std::make_unique<LxMusicListPlaylistsTool>());
    aiToolRegistry->registerTool(std::make_unique<LxMusicPlaylistSongsTool>());
    aiToolRegistry->registerTool(std::make_unique<LxMusicPlayPlaylistTool>());

    // 注册文件工具
    aiToolRegistry->registerTool(std::make_unique<ReadTextFileTool>(m_allowedRoots));
    aiToolRegistry->registerTool(std::make_unique<ListDirectoryTool>(m_allowedRoots));
    if (toolAccessPolicy.allowFileWrite) {
        aiToolRegistry->registerTool(std::make_unique<WriteTextFileTool>(m_allowedRoots, toolAccessPolicy.maxWriteBytes));
    }
    if (toolAccessPolicy.allowCommandExecution && !toolAccessPolicy.commandWhitelist.isEmpty()) {
        CommandExecutionPolicy commandPolicy;
        commandPolicy.allowedRoots = m_allowedRoots;
        commandPolicy.commandWhitelist = toolAccessPolicy.commandWhitelist;
        commandPolicy.timeoutMs = toolAccessPolicy.commandTimeoutMs;
        aiToolRegistry->registerTool(std::make_unique<ExecuteWhitelistedCommandTool>(commandPolicy));
    }

    // 注册网络工具
    aiToolRegistry->registerTool(std::make_unique<WebFetchTool>());
    aiToolRegistry->registerTool(std::make_unique<WebSearchTool>());

    // 注册生活助理工具
    aiToolRegistry->registerTool(std::make_unique<WeatherQueryTool>());
    aiToolRegistry->registerTool(std::make_unique<HolidayQueryTool>());
    aiToolRegistry->registerTool(std::make_unique<DailyBriefingTool>());

    agentScheduler->setToolRegistry(aiToolRegistry.get());
    agentScheduler->load();
    connect(agentScheduler.get(), &AgentScheduler::taskTriggered, this, [](const QString& id, const QString& title) {
        qDebug() << "[AgentScheduler] task triggered:" << id << title;
    });
    connect(agentScheduler.get(), &AgentScheduler::taskFailed, this, [](const QString& id, const QString& errorMessage) {
        qWarning() << "[AgentScheduler] task failed:" << id << errorMessage;
    });
    agentScheduler->start();

    aiBrain->setPetName(modelName);
    aiBrain->setToolRegistry(aiToolRegistry.get());
    aiBrain->setEnabled(aiEnabled);

    if (aiEnabled) {
        aiBrain->start();
        qDebug() << "[AIBrain] started for pet:" << modelName;
    }
}

