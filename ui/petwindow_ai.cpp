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
#include "ai/integration/emotion_state_provider.h"
#include "ai/skill/skill_tools.h"
#include "ai/prompt/prompt_template_store.h"
#include "ai/runtime/agent_bootstrap.h"
#include "ai/runtime/agent_runtime_services.h"
#include "ai/reflection/reflection_types.h"
#include "ai/reflection/sleep_cycle_coordinator.h"
#include "ai/runtime/runtime_ui_bridge.h"
#include "configLoader/config_manager.h"
#include "controller/pet_controller.h"
#include "presentation/avatar_presentation.h"


#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QMetaObject>
#include <QThread>

#include <iostream>
#include <utility>

namespace {
template<class Tool,class... Args>
void registerBuiltin(ToolRegistry* registry,HostToolMetadata metadata,Args... args) {
    ToolRegistry::WorkerFactory factory;
    if (metadata.executor==HostToolExecutor::Worker)
        factory=[args...] { return std::make_unique<Tool>(args...); };
    registry->registerTool(std::make_unique<Tool>(args...),metadata,std::move(factory));
}
HostToolMetadata scopedBuiltin(bool write,const QString& pathArgument) {
    auto metadata=write?HostToolMetadata::localAction(HostToolExecutor::Worker):HostToolMetadata::safeRead(HostToolExecutor::Worker);
    metadata.risk=write?ToolRiskLevel::L3HighRiskAction:ToolRiskLevel::L1LocalQuery;
    metadata.taskAllowed=!write; metadata.scopePathArgument=pathArgument; return metadata;
}
}

void PetWindow::setupAiBrain() {
    std::cerr << "[AIBrain] setup started" << std::endl;
    if (!aiBrain) {
        qWarning() << "[AIBrain] setup skipped: AI brain is unavailable";
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
        std::move(uiCallbacks), avatarPresentation.get());
    runtimeServices = std::make_unique<AgentRuntimeServices>();
    agentScheduler = std::make_unique<AgentScheduler>(this);
    QString schedulerError;
    if (!agentScheduler->configureProfileStorage(profileMigration.appDataRoot,
            profileMigration.profileId, profileMigration.registeredProfileIds, &schedulerError)) {
        qWarning() << "[AgentScheduler]" << schedulerError;
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
    runtimeRequest.ownerDiaryBootstrapPath = ownerDiaryBootstrapPath;
    runtimeRequest.emotionStateProvider = emotionStateProvider.get();
    runtimeRequest.agentScheduler = agentScheduler.get();
    runtimeRequest.aiBrain = aiBrain.get();
    runtimeRequest.uiBridge = runtimeUiBridge.get();
    const Result<RuntimeStartReport, DomainError> runtimeStarted =
        AgentBootstrap::start(*runtimeServices, runtimeRequest);
    if (!runtimeStarted.isOk()) {
        std::cerr << "[AIBrain] runtime bootstrap failed: "
                  << runtimeStarted.error().code.toStdString() << " "
                  << runtimeStarted.error().message.toStdString() << std::endl;
        qWarning() << "[AIBrain] runtime bootstrap failed:"
                   << runtimeStarted.error().code << runtimeStarted.error().message;
        runtimeServices.reset();
        runtimeUiBridge.reset();
        agentScheduler.reset();
        return;
    }
    const RuntimeStartReport& runtimeReport = runtimeStarted.value();
    if (!runtimeReport.diagnostics.isEmpty()) {
        qWarning() << "[AIBrain] runtime startup diagnostics:"
                   << runtimeReport.diagnostics;
    }
    std::cerr << "[AIBrain] storage initialized; runtime mode="
              << (runtimeReport.mode == RuntimeMode::Running
                      ? "running" : "degraded")
              << std::endl;

    // 设置文件/命令工具允许的根目录。默认来自配置，未配置时退回到应用目录和当前工作目录。
    const AiToolAccessPolicy& toolAccessPolicy = config.getAiToolAccessPolicy();
    m_allowedRoots.clear();
    m_allowedRoots.append(toolAccessPolicy.allowedRoots);
    if (m_allowedRoots.isEmpty()) {
        m_allowedRoots.append(QCoreApplication::applicationDirPath());
        m_allowedRoots.append(QDir::currentPath());
    }

    aiToolRegistry = std::make_unique<ToolRegistry>();
    registerBuiltin<ShowChatBubbleTool>(aiToolRegistry.get(), HostToolMetadata::localAction(HostToolExecutor::OwnerThread), [window](const QString& text, int durationMs) {
        if (!window) return false;
        const auto show = [window, text, durationMs]() {
            if (!window || !window->showToolBubbleMessage(text, durationMs)) return false;
            window->speakPetReply(text, QStringLiteral("toolBubble"));
            return true;
        };
        if (QThread::currentThread() == window->thread()) return show();
        bool accepted = false;
        const bool dispatched = QMetaObject::invokeMethod(window.data(),
            [&accepted, show]() { accepted = show(); }, Qt::BlockingQueuedConnection);
        return dispatched && accepted;
    });
    registerBuiltin<NotifyUserTool>(aiToolRegistry.get(), HostToolMetadata::localAction(HostToolExecutor::OwnerThread), [this](const QString& title, const QString& message, int durationMs) {
        const QString bubbleText = title.trimmed().isEmpty()
            ? message
            : QString("%1：%2").arg(title, message);
        QMetaObject::invokeMethod(this, [this, bubbleText, durationMs]() {
            showBubbleMessage(bubbleText, durationMs);
            speakPetReply(bubbleText, QStringLiteral("toolBubble"));
        }, Qt::QueuedConnection);
    });
    registerBuiltin<SetProactiveModeTool>(aiToolRegistry.get(), HostToolMetadata::localAction(HostToolExecutor::OwnerThread), [this](const QString& mode, int quietMinutes) {
        const QString text = mode == "focus" || mode == "quiet"
            ? (quietMinutes > 0 ? QStringLiteral("好，接下来 %1 分钟我会安静陪着你。").arg(quietMinutes)
                                : QStringLiteral("好，我会安静陪着你，需要我时叫我。"))
            : QStringLiteral("主动模式已切换为 %1。").arg(mode);
        QMetaObject::invokeMethod(this, [this, text]() {
            showBubbleMessage(text, 3000);
            speakPetReply(text, QStringLiteral("toolBubble"));
        }, Qt::QueuedConnection);
    }, aiBrain->proactiveStatePath());
    MemoryStore* memoryStore = aiBrain->memoryStore();
    connectSchedulerMemory(*agentScheduler, *memoryStore);
    registerBuiltin<MemoryOrganizeTool>(aiToolRegistry.get(), HostToolMetadata{"write",ToolRiskLevel::L2LowRiskAction,false,false,HostToolExecutor::OwnerThread,{}}, memoryStore);
    SkillStore* skillStore = aiBrain->skillStore();
    registerBuiltin<SkillCreateTool>(aiToolRegistry.get(), HostToolMetadata{"write",ToolRiskLevel::L2LowRiskAction,false,false,HostToolExecutor::OwnerThread,{}}, skillStore);
    registerBuiltin<SkillUpdateTool>(aiToolRegistry.get(), HostToolMetadata{"write",ToolRiskLevel::L2LowRiskAction,false,false,HostToolExecutor::OwnerThread,{}}, skillStore);
    registerBuiltin<SkillListTool>(aiToolRegistry.get(), HostToolMetadata{"read",ToolRiskLevel::L2LowRiskAction,false,false,HostToolExecutor::OwnerThread,{}}, skillStore);
    registerBuiltin<SkillDeleteTool>(aiToolRegistry.get(), HostToolMetadata{"write",ToolRiskLevel::L4Dangerous,false,false,HostToolExecutor::OwnerThread,{}}, skillStore);
    registerBuiltin<SkillRecordOutcomeTool>(aiToolRegistry.get(), HostToolMetadata{"write",ToolRiskLevel::L2LowRiskAction,false,false,HostToolExecutor::OwnerThread,{}}, skillStore);
    registerBuiltin<ScheduleCreateTool>(aiToolRegistry.get(), HostToolMetadata::localAction(HostToolExecutor::OwnerThread), agentScheduler.get(), memoryStore);
    registerBuiltin<ScheduleListTool>(aiToolRegistry.get(), HostToolMetadata::safeRead(HostToolExecutor::OwnerThread), agentScheduler.get());
    registerBuiltin<ScheduleCancelTool>(aiToolRegistry.get(), HostToolMetadata::localAction(HostToolExecutor::OwnerThread), agentScheduler.get(), memoryStore);
    registerBuiltin<ScheduleSnoozeTool>(aiToolRegistry.get(), HostToolMetadata::localAction(HostToolExecutor::OwnerThread), agentScheduler.get(), memoryStore);
    registerBuiltin<GetCurrentTimeTool>(aiToolRegistry.get(), HostToolMetadata::safeRead(HostToolExecutor::Worker));
    registerBuiltin<GetUserIdleStateTool>(aiToolRegistry.get(), HostToolMetadata::safeRead(HostToolExecutor::Worker));
    registerBuiltin<GetBatteryStatusTool>(aiToolRegistry.get(), HostToolMetadata::safeRead(HostToolExecutor::Worker));
    registerBuiltin<GetNetworkStatusTool>(aiToolRegistry.get(), HostToolMetadata::safeRead(HostToolExecutor::Worker));
    registerBuiltin<LxMusicStatusTool>(aiToolRegistry.get(), HostToolMetadata::safeRead(HostToolExecutor::Worker));
    registerBuiltin<LxMusicLaunchTool>(aiToolRegistry.get(), HostToolMetadata{"write",ToolRiskLevel::L3HighRiskAction,false,false,HostToolExecutor::Worker,{}});
    registerBuiltin<LxMusicPlayTool>(aiToolRegistry.get(), HostToolMetadata::localAction(HostToolExecutor::Worker));
    registerBuiltin<LxMusicPauseTool>(aiToolRegistry.get(), HostToolMetadata::localAction(HostToolExecutor::Worker));
    registerBuiltin<LxMusicSkipNextTool>(aiToolRegistry.get(), HostToolMetadata::localAction(HostToolExecutor::Worker));
    registerBuiltin<LxMusicSkipPrevTool>(aiToolRegistry.get(), HostToolMetadata::localAction(HostToolExecutor::Worker));
    registerBuiltin<LxMusicLyricTool>(aiToolRegistry.get(), HostToolMetadata::safeRead(HostToolExecutor::Worker));
    registerBuiltin<LxMusicVolumeTool>(aiToolRegistry.get(), HostToolMetadata::localAction(HostToolExecutor::Worker));
    registerBuiltin<LxMusicSearchPlayTool>(aiToolRegistry.get(), HostToolMetadata::localAction(HostToolExecutor::Worker));
    registerBuiltin<LxMusicListPlaylistsTool>(aiToolRegistry.get(), HostToolMetadata::safeRead(HostToolExecutor::Worker));
    registerBuiltin<LxMusicPlaylistSongsTool>(aiToolRegistry.get(), HostToolMetadata::safeRead(HostToolExecutor::Worker));
    registerBuiltin<LxMusicPlayPlaylistTool>(aiToolRegistry.get(), HostToolMetadata::localAction(HostToolExecutor::Worker));

    // 注册文件工具
    registerBuiltin<ReadTextFileTool>(aiToolRegistry.get(), scopedBuiltin(false,"path"), m_allowedRoots);
    registerBuiltin<ListDirectoryTool>(aiToolRegistry.get(), scopedBuiltin(false,"path"), m_allowedRoots);
    if (toolAccessPolicy.allowFileWrite) {
        registerBuiltin<WriteTextFileTool>(aiToolRegistry.get(), scopedBuiltin(true,"path"), m_allowedRoots, toolAccessPolicy.maxWriteBytes);
    }
    if (toolAccessPolicy.allowCommandExecution && !toolAccessPolicy.commandWhitelist.isEmpty()) {
        CommandExecutionPolicy commandPolicy;
        commandPolicy.allowedRoots = m_allowedRoots;
        commandPolicy.commandWhitelist = toolAccessPolicy.commandWhitelist;
        commandPolicy.timeoutMs = toolAccessPolicy.commandTimeoutMs;
        registerBuiltin<ExecuteWhitelistedCommandTool>(aiToolRegistry.get(), scopedBuiltin(true,"working_directory"), commandPolicy);
    }

    // 注册网络工具
    registerBuiltin<WebFetchTool>(aiToolRegistry.get(), HostToolMetadata::safeRead(HostToolExecutor::Worker));
    registerBuiltin<WebSearchTool>(aiToolRegistry.get(), HostToolMetadata::safeRead(HostToolExecutor::Worker));

    // 注册生活助理工具
    registerBuiltin<WeatherQueryTool>(aiToolRegistry.get(), HostToolMetadata::safeRead(HostToolExecutor::Worker));
    registerBuiltin<HolidayQueryTool>(aiToolRegistry.get(), HostToolMetadata::safeRead(HostToolExecutor::Worker));
    registerBuiltin<DailyBriefingTool>(aiToolRegistry.get(), HostToolMetadata::safeRead(HostToolExecutor::Worker));

    agentScheduler->setToolRegistry(aiToolRegistry.get());
    agentScheduler->setUserBusyProvider([this]() {
        const int idle = aiBrain ? aiBrain->userIdleSeconds() : -1;
        return idle >= 0 && idle < 60;
    });
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
    connect(agentScheduler.get(), &AgentScheduler::taskPartiallySucceeded, this,
            [](const QString& id, const QString& detail) {
                qWarning() << "[AgentScheduler] reminder delivered with optional action failure:" << id << detail;
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

    setupAiAnimationTools();
    QPointer<AvatarPresentation> avatar(avatarPresentation.get());
    aiBrain->setAvatarContextProvider([avatar] {
        if (!avatar) return QJsonObject{};
        return QJsonObject{{"state", avatar->snapshot()}, {"avatar", avatar->capabilities()["avatar"]}};
    });
    aiBrain->setToolRegistry(aiToolRegistry.get());
    aiBrain->setAgentScheduler(agentScheduler.get());
    aiBrain->setEnabled(aiEnabled);

    if (aiEnabled) {
        aiBrain->start();
        qInfo() << "[AIBrain] started for pet:" << modelName;
    }
    std::cerr << "[AIBrain] setup completed; awaiting runtime settings"
              << std::endl;
}

void PetWindow::setupAiAnimationTools() {
    if (!aiToolRegistry || !runtimeUiBridge) return;
    auto* presentation = runtimeUiBridge->presentation();
    registerBuiltin<PlayAnimationTool>(aiToolRegistry.get(), HostToolMetadata::localAction(HostToolExecutor::OwnerThread), presentation);
    registerBuiltin<GetCurrentAnimationTool>(aiToolRegistry.get(), HostToolMetadata::safeRead(HostToolExecutor::OwnerThread), presentation);
    registerBuiltin<GetIdleTransitionCandidatesTool>(aiToolRegistry.get(), HostToolMetadata::safeRead(HostToolExecutor::OwnerThread), presentation);
    registerBuiltin<GetActionTransitionStatusTool>(aiToolRegistry.get(), HostToolMetadata::safeRead(HostToolExecutor::OwnerThread), presentation);
    registerBuiltin<RequestIdleTransitionTool>(aiToolRegistry.get(), HostToolMetadata::localAction(HostToolExecutor::OwnerThread), presentation);
    registerBuiltin<CancelAnimationRequestTool>(aiToolRegistry.get(), HostToolMetadata::localAction(HostToolExecutor::OwnerThread), presentation);
    registerBuiltin<GetAvatarCapabilitiesTool>(aiToolRegistry.get(), HostToolMetadata::safeRead(HostToolExecutor::OwnerThread), presentation);
    registerBuiltin<MoveAvatarTool>(aiToolRegistry.get(), HostToolMetadata::localAction(HostToolExecutor::OwnerThread), presentation);
}

void PetWindow::teardownAiRuntime() {
    if (agentScheduler) {
        agentScheduler->stop();
    }
    if (runtimeServices) {
        runtimeServices->stop();
        runtimeServices.reset();
    }
    runtimeUiBridge.reset();
    emotionStateProvider.reset();

    if (aiBrain) {
        aiBrain->setAgentScheduler(nullptr);
        aiBrain->setToolRegistry(nullptr);
        aiBrain->stop();
    }
    agentScheduler.reset();
    aiToolRegistry.reset();
    aiBrain.reset();
}

void PetWindow::onManualDaydreamRequested() {
    if (!aiBrain) {
        showBubbleMessage(QStringLiteral("记忆整理功能未就绪"), 3000);
        return;
    }
    const auto result = aiBrain->requestManualDaydream();
    showBubbleMessage(result.isOk() ? QStringLiteral("已安排后台记忆整理，可以继续聊天。")
                                   : result.error().message, 3000);
}
