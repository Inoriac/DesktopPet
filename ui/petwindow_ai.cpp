//
// Created by Inoriac on 2025/10/15.
//

#include "petwindow.h"

#include "ai/tools/animation_tools.h"
#include "ai/tools/companion_tools.h"
#include "ai/tools/environment_tools.h"
#include "ai/tools/file_tools.h"
#include "ai/tools/life_tools.h"
#include "ai/tools/memory_tools.h"
#include "ai/tools/music_tools.h"
#include "ai/tools/schedule_tools.h"
#include "ai/tools/web_tools.h"
#include "ai/skill/skill_tools.h"
#include "ai/prompt/prompt_template_store.h"
#include "ai/runtime/agent_bootstrap.h"
#include "ai/runtime/agent_runtime_services.h"
#include "ai/runtime/runtime_ui_bridge.h"
#include "configLoader/config_manager.h"
#include "controller/pet_controller.h"
#include "render_engine.h"
#include "render_viewport.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QMetaObject>

#include <utility>

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
    if (runtimeServices || runtimeUiBridge) {
        qWarning() << "[AIBrain] runtime is already configured, skip duplicate setup";
        return;
    }

    QPointer<PetWindow> window(this);
    RuntimeUiCallbacks uiCallbacks;
    uiCallbacks.showChatBubble = [window](const QString& text, int durationMs) {
        if (!window) return;
        QMetaObject::invokeMethod(window.data(), [window, text, durationMs]() {
            if (!window) return;
            window->showBubbleMessage(text, durationMs);
            window->speakPetReply(text, QStringLiteral("toolBubble"));
        }, Qt::QueuedConnection);
    };
    uiCallbacks.notifyUser =
        [window](const QString& title, const QString& message, int durationMs) {
            if (!window) return;
            const QString bubbleText = title.trimmed().isEmpty()
                ? message
                : QStringLiteral("%1：%2").arg(title, message);
            QMetaObject::invokeMethod(window.data(), [window, bubbleText, durationMs]() {
                if (!window) return;
                window->showBubbleMessage(bubbleText, durationMs);
                window->speakPetReply(bubbleText, QStringLiteral("toolBubble"));
            }, Qt::QueuedConnection);
        };
    runtimeUiBridge = std::make_unique<CallbackRuntimeUiBridge>(
        std::move(uiCallbacks), player, animationManager);
    runtimeServices = std::make_unique<AgentRuntimeServices>();
    agentScheduler = std::make_unique<AgentScheduler>(this);
    if (!agentScheduler->load()) {
        qWarning() << "[AgentScheduler] failed to load persisted tasks";
    }

    ConfigManager& config = ConfigManager::instance();
    RuntimeStartRequest runtimeRequest;
    runtimeRequest.profile = profile;
    runtimeRequest.profileMigration = profileMigration;
    runtimeRequest.configHash = config.configHash();
    runtimeRequest.identityBaselineSchemaVersion = config.getIdentityBaseline().schemaVersion;
    runtimeRequest.identityBaselineHash = config.identityBaselineHash();
    runtimeRequest.identityBaseline = config.getIdentityBaseline();
    runtimeRequest.personalityPolicy = config.getPersonalityPolicy();
    runtimeRequest.sleepPolicy = config.getSleepPolicy();
    runtimeRequest.emotionStateProvider = emotionStateProvider.get();
    runtimeRequest.agentScheduler = agentScheduler.get();
    runtimeRequest.aiBrain = aiBrain.get();
    runtimeRequest.uiBridge = runtimeUiBridge.get();
    const Result<RuntimeStartReport, DomainError> runtimeStarted =
        AgentBootstrap::start(*runtimeServices, runtimeRequest);
    if (!runtimeStarted.isOk()) {
        qWarning() << "[AIBrain] runtime bootstrap failed:"
                   << runtimeStarted.error().code << runtimeStarted.error().message;
        runtimeServices.reset();
        runtimeUiBridge.reset();
        agentScheduler.reset();
        return;
    }
    const RuntimeStartReport& runtimeReport = runtimeStarted.value();
    if (runtimeReport.mode == RuntimeMode::Degraded) {
        qWarning() << "[AIBrain] runtime started in degraded mode:"
                   << runtimeReport.diagnostics;
    }

    // 设置文件/命令工具允许的根目录。默认来自配置，未配置时退回到应用目录和当前工作目录。
    const AiToolAccessPolicy& toolAccessPolicy = config.getAiToolAccessPolicy();
    m_allowedRoots.clear();
    m_allowedRoots.append(toolAccessPolicy.allowedRoots);
    if (m_allowedRoots.isEmpty()) {
        m_allowedRoots.append(QCoreApplication::applicationDirPath());
        m_allowedRoots.append(QDir::currentPath());
    }

    aiToolRegistry = std::make_unique<ToolRegistry>();
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
    MemoryStore* memoryStore = aiBrain->memoryStore();
    aiToolRegistry->registerTool(std::make_unique<MemoryOrganizeTool>(memoryStore));
    SkillStore* skillStore = aiBrain->skillStore();
    aiToolRegistry->registerTool(std::make_unique<SkillCreateTool>(skillStore));
    aiToolRegistry->registerTool(std::make_unique<SkillUpdateTool>(skillStore));
    aiToolRegistry->registerTool(std::make_unique<SkillListTool>(skillStore));
    aiToolRegistry->registerTool(std::make_unique<SkillDeleteTool>(skillStore));
    aiToolRegistry->registerTool(std::make_unique<SkillRecordOutcomeTool>(skillStore));
    aiToolRegistry->registerTool(std::make_unique<ScheduleCreateTool>(agentScheduler.get(), memoryStore));
    aiToolRegistry->registerTool(std::make_unique<ScheduleListTool>(agentScheduler.get()));
    aiToolRegistry->registerTool(std::make_unique<ScheduleCancelTool>(agentScheduler.get(), memoryStore));
    aiToolRegistry->registerTool(std::make_unique<ScheduleSnoozeTool>(agentScheduler.get(), memoryStore));
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
    aiToolRegistry->registerTool(std::make_unique<LxMusicLaunchTool>());
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
    connect(agentScheduler.get(), &AgentScheduler::taskTriggered, this, [this](const QString& id, const QString& title) {
        qDebug() << "[AgentScheduler] task triggered:" << id << title;
        if (petController) {
            petController->recordTaskOutcome(id, true);
        }
    });
    connect(agentScheduler.get(), &AgentScheduler::taskFailed, this, [this](const QString& id, const QString& errorMessage) {
        qWarning() << "[AgentScheduler] task failed:" << id << errorMessage;
        if (petController && !id.trimmed().isEmpty()) {
            petController->recordTaskOutcome(id, false);
        }
    });
    agentScheduler->start();

    aiBrain->setPetName(modelName);

    // 注入通用提示词模版 + 独立身份基线：系统提示词渲染的输入。
    // 模版加载失败/未命中时不调用 setPromptTemplate，ContextBuilder 回退内联兜底模版（零回归）。
    PromptTemplateStore promptTemplateStore;
    promptTemplateStore.setStoragePath(QStringLiteral("config/prompts"));
    if (promptTemplateStore.load()) {
        const PromptTemplate* templ =
            promptTemplateStore.findByName(ConfigManager::instance().activePromptTemplateName());
        if (templ) {
            aiBrain->setPromptTemplate(*templ);
        } else {
            qWarning() << "[AIBrain] 未找到提示词模版，使用内联兜底:"
                       << ConfigManager::instance().activePromptTemplateName();
        }
    } else {
        qWarning() << "[AIBrain] 提示词模版目录未加载，使用内联兜底模版";
    }
    aiBrain->setIdentityBaseline(ConfigManager::instance().getIdentityBaseline());

    aiBrain->setToolRegistry(aiToolRegistry.get());
    aiBrain->setAgentScheduler(agentScheduler.get());
    aiBrain->setEnabled(aiEnabled);

    if (aiEnabled) {
        aiBrain->start();
        qDebug() << "[AIBrain] started for pet:" << modelName;
    }
}

void PetWindow::teardownAiRuntime() {
    if (runtimeServices) {
        runtimeServices->stop();
        runtimeServices.reset();
    }
    runtimeUiBridge.reset();
    emotionStateProvider.reset();

    if (agentScheduler) {
        agentScheduler->stop();
    }
    if (aiBrain) {
        aiBrain->setAgentScheduler(nullptr);
        aiBrain->setToolRegistry(nullptr);
        aiBrain->stop();
    }
    agentScheduler.reset();
    aiToolRegistry.reset();
    aiBrain.reset();
}
