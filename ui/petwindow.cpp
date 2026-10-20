#include "presentation/avatar_presentation.h"
#include <QFile>
#include <QJsonDocument>
#include <QShowEvent>
#include <QHideEvent>
#include <QSystemTrayIcon>
#include <QStyle>
//
// Created by Inoriac on 2025/10/15.
//

#include "petwindow.h"
#include "chat_conversation_model.h"
#include "liquidglasschatbubble.h"
#include "render_viewport.h"

#include <QApplication>
#include <qboxlayout.h>
#include <QDebug>
#include <QKeyEvent>
#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QMessageBox>
#include <QScreen>
#include <QGuiApplication>
#include <QDateTime>
#include <QRandomGenerator>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QBuffer>
#include <QDir>
#include <QRegularExpression>
#include <QUrl>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

#include "behavior/behavior_manager.h"
#include "configLoader/config_manager.h"
#include "controller/pet_controller.h"
#include "emotion/emotion_engine.h"
#include "emotion/sqlite_emotion_state_repository.h"
#include "ai/tools/animation_tools.h"
#include "ai/tools/companion_tools.h"
#include "ai/tools/environment_tools.h"
#include "ai/tools/life_tools.h"
#include "ai/tools/music_tools.h"
#include "ai/tools/schedule_tools.h"
#include "ai/tools/file_tools.h"
#include "ai/tools/web_tools.h"
#include "ai/integration/emotion_state_provider.h"
#include "ai/runtime/agent_runtime_services.h"
#include "ai/runtime/runtime_ui_bridge.h"
#include "ai/runtime/runtime_overview.h"
#include "ai/reflection/sleep_cycle_coordinator.h"
#include "ai/chat/launcher_chat_server.h"
#include "ai/owner/owner_diary_protocol.h"
#include "statistic_manager.h"

namespace {

QJsonObject launcherChatStatistics(const QString& petName) {
    const auto statistics =
        StatisticManager::getInstance().getPetStatistics(petName);
    if (!statistics.has_value()) return {};
    const PetStatistics& stats = *statistics;
    return {
        {QStringLiteral("callCount"), stats.llmCallCount},
        {QStringLiteral("successCount"), stats.llmSuccessCount},
        {QStringLiteral("failureCount"), stats.llmFailureCount},
        {QStringLiteral("promptTokens"), stats.llmPromptTokens},
        {QStringLiteral("completionTokens"), stats.llmCompletionTokens},
        {QStringLiteral("totalTokens"), stats.llmTotalTokens},
        {QStringLiteral("reasoningTokens"), stats.llmReasoningTokens},
        {QStringLiteral("cachedTokens"), stats.llmCachedTokens}
    };
}

} // namespace


PetWindow::PetWindow(PetProfile profile,
                     ProfileMigrationRequest profileMigration,
                     QString ownerDiaryBootstrapPath,
                     QString launcherChatBootstrapPath,
                     QWidget *parent)
    : QWidget(parent)
    , isDragging(false)
    , renderViewport(nullptr)
    , profile(std::move(profile))
    , profileMigration(std::move(profileMigration))
    , ownerDiaryBootstrapPath(std::move(ownerDiaryBootstrapPath))
    , launcherChatBootstrapPath(std::move(launcherChatBootstrapPath))
    , modelName(this->profile.name)
    , sizePercent(100)
    , alwaysOnTop(true)
    , clickThrough(false)
    , contextMenu(nullptr)
    , closeAction(nullptr) {
    ConfigManager& config = ConfigManager::instance();
    snapThreshold = config.getWindowSnapThreshold();
    snapVerticalOffset = config.getWindowSnapVerticalOffset();
    snapZoneOffset = config.getWindowSnapZoneOffset();
    snapZoneSize = config.getWindowSnapZoneSize();
    snapFollowIntervalMs = config.getWindowSnapFollowIntervalMs();
    forceExitOnBigScreenAlarm = config.getWindowSnapForceExitOnBigScreenAlarm();
    totalWindowSitAnimations = config.getTotalWindowSitAnimations();

    setupWindow();
    setupRenderViewport();
    setupEmotionSystem();
    emotionStateProvider = std::make_unique<EmotionEngineStateProvider>(emotionEngine.get());
    setupContextMenu();
    setupWindowSnapping();
    setupDropAnimation();
    setupScreenChat();

    conversationModel = std::make_unique<ChatConversationModel>();
    conversationModel->setDeferredPersistence(true);
    connect(conversationModel.get(),
            &ChatConversationModel::historyPersistenceWarning,
            this,
            [](const QString& safeMessage) {
                qWarning() << "[Chat] history persistence warning:"
                           << safeMessage;
            });
    ProfileChatStoreOptions chatStoreOptions;
    chatStoreOptions.appDataRoot = this->profileMigration.appDataRoot;
    chatStoreOptions.profileId = this->profileMigration.profileId;
    chatStoreOptions.runtimeDatabasePath = QDir(this->profileMigration.appDataRoot).filePath(
        QStringLiteral("profiles/%1/agent_runtime.sqlite").arg(this->profileMigration.profileId));
    chatStoreOptions.registeredProfileIds =
        this->profileMigration.registeredProfileIds;
    chatStoreOptions.legacyHistoryPath = QDir::current().filePath(
        QStringLiteral("log/chat_history.jsonl"));
    QString chatHistoryError;
    if (!conversationModel->initialize(chatStoreOptions, &chatHistoryError)) {
        qWarning() << "[Chat] unable to initialize conversation model:"
                   << chatHistoryError;
    } else if (!chatHistoryError.isEmpty()) {
        qWarning() << "[Chat] conversation is using degraded persistence:"
                   << chatHistoryError;
    }

    aiBrain = std::make_unique<AIBrain>(this);
    aiBrain->restoreConversationHistory(conversationModel->messages());
    aiBrain->setEmotionSnapshotProvider([this]() -> std::optional<EmotionSnapshot> {
        if (!emotionEngine || !emotionEngine->isEnabled()) {
            return std::nullopt;
        }
        return emotionEngine->snapshot(QDateTime::currentDateTimeUtc());
    });
    connect(aiBrain.get(), &AIBrain::thinkRequestRejected, this,
            [this](const QString& replyToId, const QString& errorMessage) {
                qWarning() << "[AIBrain] chat request could not start:" << errorMessage;
                if (!conversationModel || replyToId.isEmpty()) return;
                const QString assistantId =
                    QUuid::createUuid().toString(QUuid::WithoutBraces);
                conversationModel->beginAssistantMessage(assistantId, replyToId);
                conversationModel->finishAssistantMessage(
                    assistantId, ChatMessageStatus::Failed, errorMessage);
                showChatErrorNotification();
            });
    connect(aiBrain.get(), &AIBrain::assistantResponseReady, this, [this](const QString& content, const QString& voiceSource) {
        qDebug() << "[AIBrain] assistant response:" << content;
        showBubbleMessage(content);
        speakPetReply(content, voiceSource);
    });
    connect(aiBrain.get(), &AIBrain::assistantResponseStarted,
            conversationModel.get(),
            [this](const QString& messageId,
                   const QString& replyToId,
                   const QString&) {
                conversationModel->beginAssistantMessage(messageId, replyToId,
                    QDateTime::currentDateTime(), aiBrain->activeUserMessageIds());
            });
    connect(aiBrain.get(), &AIBrain::assistantResponseStageChanged,
            conversationModel.get(),
            [this](const QString& messageId, ChatActivityStage stage) {
                conversationModel->setAssistantStage(messageId, stage);
            });
    connect(aiBrain.get(), &AIBrain::assistantResponseDelta,
            conversationModel.get(),
            [this](const QString& messageId, const QString& textDelta) {
                conversationModel->appendAssistantDelta(messageId, textDelta);
            });
    connect(aiBrain.get(), &AIBrain::assistantResponseFinished,
            conversationModel.get(),
            [this](const QString& messageId,
                   ChatMessageStatus status,
                   const QString& errorMessage) {
                conversationModel->finishAssistantMessage(
                    messageId, status, errorMessage);
                if (status == ChatMessageStatus::Failed
                    || status == ChatMessageStatus::Interrupted) {
                    showChatErrorNotification();
                }
            });
    connect(aiBrain.get(), &AIBrain::toolExecuted, this, [this](const QString& toolName, bool success, const QString& payload) {
        qDebug() << "[AIBrain] tool executed:" << toolName << "success:" << success << "payload:" << payload;
        Q_UNUSED(payload);
        if (petController) {
            petController->recordToolOutcome(toolName, success);
        }
    });

    connect(aiBrain.get(), &AIBrain::toolConfirmationRequired, this,
            [this](const QString& requestId,
                   const QString& toolName,
                   const QString& reason,
                   const QJsonObject& arguments) {
        QStringList targets;
        for (const QString& key : {QStringLiteral("path"),
                                   QStringLiteral("working_directory"),
                                   QStringLiteral("command"),
                                   QStringLiteral("url")}) {
            if (arguments.contains(key)) {
                targets.append(QStringLiteral("%1: %2").arg(key, arguments.value(key).toVariant().toString()));
            }
        }
        QString argumentText = QString::fromUtf8(
            QJsonDocument(arguments).toJson(QJsonDocument::Indented));
        constexpr int maxArgumentDisplayLength = 2000;
        if (argumentText.size() > maxArgumentDisplayLength) {
            argumentText = argumentText.left(maxArgumentDisplayLength)
                + QStringLiteral("\n... (参数已截断)");
        }
        const QString targetText = targets.isEmpty()
            ? QStringLiteral("未提供路径或命令目标")
            : targets.join('\n');
        const QString message = QStringLiteral(
                                    "桌宠请求执行操作：%1\n\n%2\n\n目标：\n%3\n\n操作参数：\n%4\n\n是否允许本次执行？")
                                    .arg(toolName, reason, targetText, argumentText);
        const bool approved = QMessageBox::question(
                                  this,
                                  QStringLiteral("确认工具操作"),
                                  message,
                                  QMessageBox::Yes | QMessageBox::No,
                                  QMessageBox::No) == QMessageBox::Yes;
        if (aiBrain) {
            aiBrain->resolveToolConfirmation(requestId, approved);
        }
    });

    // Basic dialogue must be ready before the synchronous model/animation
    // import starts. Animation-specific tools are attached after model load.
    setupAiBrain();
    setupLauncherChatBridge();
}

PetWindow::~PetWindow() {
    if (aiBrain) aiBrain->stop();
    voiceSynthesis.stop();
    if (snapFollowTimer) {
        snapFollowTimer->stop();
    }
    if (snapScanTimer) {
        snapScanTimer->stop();
    }
    if (dropTimer) {
        dropTimer->stop();
    }
    if (emotionTickTimer) {
        emotionTickTimer->stop();
    }
    if (screenChatTimer) {
        screenChatTimer->stop();
    }
    if (bubbleHideTimer) {
        bubbleHideTimer->stop();
    }
    if (thinkingBubbleTimer) {
        thinkingBubbleTimer->stop();
    }
    if (outputBubble) {
        outputBubble->close();
        outputBubble->deleteLater();
        outputBubble = nullptr;
    }
    if (inputBubble) {
        inputBubble->close();
        inputBubble->deleteLater();
        inputBubble = nullptr;
    }
    launcherChatServer.reset();
    teardownAiRuntime();
    if (renderViewport) {
        delete renderViewport;
    }
    if (contextMenu) {
        delete contextMenu;
    }
}

void PetWindow::applySettings(int sizePercent,
                              bool alwaysOnTop,
                              bool clickThrough,
                              bool aiEnabled,
                              const ScreenChatConfig& screenChatConfig,
                              const VoiceConfig& voiceConfig) {
    this->alwaysOnTop = alwaysOnTop;
    this->clickThrough = clickThrough;

    // 更新窗口标志
    updateWindowFlags(alwaysOnTop, clickThrough);

    applyRuntimeSettings(sizePercent, aiEnabled, screenChatConfig, voiceConfig);
}

void PetWindow::applyRuntimeSettings(int sizePercent,
                                     bool aiEnabled,
                                     const ScreenChatConfig& screenChatConfig,
                                     const VoiceConfig& voiceConfig) {
    this->sizePercent = sizePercent;
    this->aiEnabled = aiEnabled;
    this->screenChatConfig = screenChatConfig;
    this->voiceConfig = voiceConfig;
    voiceSynthesis.setConfig(this->voiceConfig);

    // 更新大小
    int baseSize = 400;
    int newSize = baseSize * sizePercent / 100;
    if (size() != QSize(newSize, newSize)) {
        resize(newSize, newSize);
    }

    qDebug() << "PetWindow runtime setting applied - size:" << newSize;
    qDebug() << "AI enabled:" << this->aiEnabled;
    qDebug() << "Screen chat enabled:" << this->screenChatConfig.enabled;
    qDebug() << "Voice enabled:" << this->voiceConfig.enabled;

    if (outputBubble) {
        outputBubble->applyScreenChatConfig(this->screenChatConfig);
    }
    if (inputBubble) {
        inputBubble->applyScreenChatConfig(this->screenChatConfig);
    }
    updateBubblePositions();
    updateScreenChatSchedule();

    if (aiBrain && aiBrain->isStorageInitialized()) {
        aiBrain->setEnabled(aiEnabled);
        if (aiEnabled) {
            aiBrain->start();
        }
    }
}

void PetWindow::previewBubble(const QString& message) {
    const QString previewText = message.trimmed().isEmpty()
        ? QStringLiteral("气泡预览：位置和样式会实时生效")
        : message.trimmed();

    showBubbleMessage(previewText);
    if (bubbleHideTimer) {
        bubbleHideTimer->start(2200);
    }
}

void PetWindow::setupLauncherChatBridge() {
    if (launcherChatBootstrapPath.trimmed().isEmpty()) return;

    std::cerr << "[LauncherChat] bootstrap option received for profile="
              << profileMigration.profileId.toStdString() << std::endl;

    auto bootstrap = consumeOwnerDiaryBootstrap(
        launcherChatBootstrapPath, profileMigration.profileId);
    launcherChatBootstrapPath.clear();
    if (!bootstrap.isOk()) {
        qWarning() << "[LauncherChat] bootstrap rejected:"
                   << bootstrap.error().message;
        return;
    }
    std::cerr << "[LauncherChat] bootstrap accepted" << std::endl;

    LauncherChatCallbacks callbacks;
    callbacks.snapshot = [this]() {
        QJsonObject snapshot = conversationModel ? conversationModel->recentHistoryPage()
                                                 : chatHistoryPage({}, false, false);
        const QJsonObject runtime{
            {QStringLiteral("petName"), modelName},
            {QStringLiteral("profileId"), profileMigration.profileId},
            {QStringLiteral("aiEnabled"), aiBrain && aiBrain->isEnabled()},
            {QStringLiteral("busy"), aiBrain && (aiBrain->isBusy() || aiBrain->pendingUserMessageCount() > 0)},
            {QStringLiteral("pendingMessageCount"), aiBrain ? aiBrain->pendingUserMessageCount() : 0},
            {QStringLiteral("task"), aiBrain ? aiBrain->taskSnapshot() : QJsonObject{}},
            {QStringLiteral("statistics"), launcherChatStatistics(modelName)}
        };
        for (auto it = runtime.constBegin(); it != runtime.constEnd(); ++it)
            snapshot.insert(it.key(), it.value());
        return snapshot;
    };
    callbacks.runtimeOverview = [this]() {
        RuntimeOverviewInput input;
        input.profileId = profileMigration.profileId;
        input.petName = modelName;
        input.capturedAt = QDateTime::currentDateTimeUtc();
        if (aiBrain) {
            input.aiEnabled = aiBrain->isEnabled();
            input.storageReady = aiBrain->isStorageInitialized();
            input.busy = aiBrain->isBusy();
            input.pendingMessageCount = aiBrain->pendingUserMessageCount();
            input.autonomy = aiBrain->autonomyStatus();
            input.taskStatus = aiBrain->taskSnapshot().value("status").toString();
            input.daydreamRunning = aiBrain->isDaydreamRunning();
        }
        input.sleeping = runtimeServices && runtimeServices->sleepCycleCoordinator()
            && runtimeServices->sleepCycleCoordinator()->isActive();
        input.emotionEnabled = emotionEngine && emotionEngine->isEnabled();
        if (input.emotionEnabled) input.emotion = emotionEngine->snapshot(input.capturedAt);
        input.statistics = StatisticManager::getInstance().overview(input.capturedAt);
        return runtimeOverviewJson(input);
    };
    callbacks.queryHistory = [this](const QString& action, const QJsonObject& request,
                                    ChatHistoryCompletion completion) {
        if (conversationModel) conversationModel->requestHistory(action, request, std::move(completion));
        else completion(ChatHistoryResult::failure(domainError("STORE_UNAVAILABLE",
            QStringLiteral("聊天记录尚未准备好。"))));
    };
    callbacks.sendMessage = [this](const QString& rawText, const QJsonArray& sources,
                                   ChatHistoryCompletion completion) {
        const QString text = rawText.trimmed();
        if ((text.isEmpty() && sources.isEmpty()) || text.size() > 8000) {
            completion(ChatHistoryResult::failure(domainError("CHAT_MESSAGE_INVALID", QStringLiteral("消息为空或超过 8000 个字符。"))));
            return;
        }
        if (!conversationModel || !aiBrain || !aiBrain->isStorageInitialized()) {
            completion(ChatHistoryResult::failure(domainError("CHAT_UNAVAILABLE", QStringLiteral("聊天运行时尚未准备好。"))));
            return;
        }
        if (!aiBrain->isEnabled()) {
            completion(ChatHistoryResult::failure(domainError("AI_DISABLED", QStringLiteral("AI 当前没有启用。"))));
            return;
        }
        if (!aiBrain->canAcceptUserMessage()) {
            completion(ChatHistoryResult::failure(domainError("CHAT_BUSY", QStringLiteral("等待中的消息过多，请稍后再试。"))));
            return;
        }
        conversationModel->appendUserMessageWithImages(text,sources,
            [this,text,completion=std::move(completion)](ChatHistoryResult saved) mutable {
                if (!saved) { completion(std::move(saved)); return; }
                if (petController) petController->recordExplicitFeedbackText(text);
                const QString id=saved.value()["messageId"].toString();
                const auto submitted=aiBrain->submitUserMessage(text,id,true,saved.value()["images"].toArray());
                if (!submitted) {
                    // The durable user message remains retryable if the runtime changed during import.
                    const auto reply=QUuid::createUuid().toString(QUuid::WithoutBraces);
                    conversationModel->beginAssistantMessage(reply,id);
                    conversationModel->finishAssistantMessage(reply,ChatMessageStatus::Failed,submitted.error().message);
                }
                completion(std::move(saved));
            });
    };
    callbacks.retryMessage = [this](const QString& assistantMessageId)
        -> Result<QJsonObject, DomainError> {
        if (!conversationModel || !aiBrain || !aiBrain->isEnabled()
            || aiBrain->isBusy() || aiBrain->pendingUserMessageCount() > 0
            || !aiBrain->canAcceptUserMessage()) {
            return Result<QJsonObject, DomainError>::failure(domainError(
                QStringLiteral("CHAT_BUSY"),
                QStringLiteral("当前无法重新生成回复。")));
        }
        const QList<ChatHistoryEntry> messages = conversationModel->messages();
        const auto assistant = std::find_if(
            messages.cbegin(), messages.cend(),
            [&assistantMessageId](const ChatHistoryEntry& entry) {
                return entry.id == assistantMessageId
                    && entry.role == QLatin1String("assistant")
                    && (entry.status == ChatMessageStatus::Failed
                        || entry.status == ChatMessageStatus::Interrupted);
            });
        if (assistant == messages.cend() || assistant->replyToId.isEmpty()) {
            return Result<QJsonObject, DomainError>::failure(domainError(
                QStringLiteral("CHAT_MESSAGE_NOT_FOUND"),
                QStringLiteral("找不到可重试的回复。")));
        }
        const QString text = conversationModel->sourceInputForReply(assistantMessageId);
        const auto images=conversationModel->sourceImagesForReply(assistantMessageId);
        if (text.isEmpty() && images.isEmpty()) {
            return Result<QJsonObject, DomainError>::failure(domainError(
                QStringLiteral("CHAT_MESSAGE_NOT_FOUND"), QStringLiteral("找不到完整的原始用户消息。")));
        }
        const QString newUserId = conversationModel->appendUserMessage(text,QDateTime::currentDateTime(),images);
        if (newUserId.isEmpty()) {
            return Result<QJsonObject, DomainError>::failure(domainError(
                QStringLiteral("CHAT_MESSAGE_INVALID"),
                QStringLiteral("消息无法加入聊天记录。")));
        }
        if (petController) petController->recordExplicitFeedbackText(text);
        const auto submitted = aiBrain->submitUserMessage(text, newUserId, false,images);
        if (!submitted.isOk()) return Result<QJsonObject, DomainError>::failure(submitted.error());
        return Result<QJsonObject, DomainError>::success({
            {QStringLiteral("messageId"), newUserId}
        });
    };
    callbacks.stopResponse = [this]() -> Result<void, DomainError> {
        if (aiBrain) aiBrain->stopCurrentResponse();
        return Result<void, DomainError>::success();
    };
    callbacks.taskAction = [this](const QString& action, const QJsonObject& payload)
        -> Result<void, DomainError> {
        if (!aiBrain || aiBrain->taskSnapshot().value("id").toString() != payload.value("taskId").toString()) {
            return Result<void, DomainError>::failure(domainError(
                QStringLiteral("TASK_NOT_FOUND"), QStringLiteral("当前任务已变化，请刷新后重试。")));
        }
        bool accepted = true;
        if (action == "resume_task") {
            TaskBudgetExtension extension;
            if (payload.value("extendBudget").toBool()) extension = {12, 2, 300000};
            accepted = aiBrain->resumeTask(payload.value("feedback").toString(), extension);
        } else if (action == "pause_task") aiBrain->pauseTask();
        else if (action == "stop_task") aiBrain->stopTask();
        else if (action == "resolve_task_call") {
            accepted = aiBrain->resolveTaskUnknownCall(payload.value("callId").toString(), payload.value("succeeded").toBool());
        } else accepted = false;
        if (!accepted) {
            return Result<void, DomainError>::failure(domainError(QStringLiteral("TASK_ACTION_UNAVAILABLE"),
                aiBrain->taskSnapshot().value("reason").toString(QStringLiteral("当前任务暂时无法继续，请查看任务进度。"))));
        }
        return Result<void, DomainError>::success();
    };

    OwnerDiaryBootstrap settings = bootstrap.takeValue();
    launcherChatServer = std::make_unique<LauncherChatServer>(
        profileMigration.profileId, std::move(callbacks));
    const auto listening = launcherChatServer->listen(
        settings.socketName, settings.capabilityToken,
        settings.maxFrameBytes, settings.sessionTtlSeconds);
    settings.capabilityToken.fill('\0');
    settings.capabilityToken.clear();
    if (!listening.isOk()) {
        qWarning() << "[LauncherChat] listener unavailable:"
                   << listening.error().message;
        launcherChatServer.reset();
        return;
    }

    auto notifyChanged = [this]() {
        if (launcherChatServer) launcherChatServer->notifyStateChanged();
    };
    connect(conversationModel.get(), &ChatConversationModel::messageInserted,
            this, [notifyChanged](int, const QString&) { notifyChanged(); });
    connect(conversationModel.get(), &ChatConversationModel::messageChanged,
            this, [notifyChanged](int, const QString&) { notifyChanged(); });
    connect(aiBrain.get(), &AIBrain::pendingUserMessagesChanged, this, notifyChanged);
    connect(aiBrain.get(), &AIBrain::taskProgressChanged,
            this, [notifyChanged](const QJsonObject&) { notifyChanged(); });
    connect(aiBrain.get(), &AIBrain::thinkingStarted,
            this, [notifyChanged](const QString&) { notifyChanged(); });
    connect(aiBrain.get(), &AIBrain::thinkingFinished,
            this, [notifyChanged](bool, const QString&) { notifyChanged(); });
    connect(&StatisticManager::getInstance(), &StatisticManager::statisticsUpdated,
            this, [this, notifyChanged](const QString& petName, const PetStatistics&) {
                if (petName == modelName) notifyChanged();
            });
    qInfo() << "[LauncherChat] bridge listening for profile"
            << profileMigration.profileId;
    std::cerr << "[LauncherChat] bridge ready for profile="
              << profileMigration.profileId.toStdString() << std::endl;
}

void PetWindow::openLauncherChat() {
    if (launcherChatServer && launcherChatServer->isListening()) {
        launcherChatServer->requestOpenInLauncher();
        return;
    }
    showBubbleMessage(QStringLiteral("请在 Launcher 中打开聊天。"), 3000);
}

void PetWindow::appendChatHistoryMessage(const QString& role,
                                         const QString& content,
                                         const QDateTime& timestamp,
                                         bool persist) {
    Q_UNUSED(persist)
    const QString trimmedContent = content.trimmed();
    if (!conversationModel || trimmedContent.isEmpty()) return;
    if (role == QLatin1String("user")) {
        conversationModel->appendUserMessage(trimmedContent, timestamp);
        return;
    }
    const QString assistantId =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    conversationModel->beginAssistantMessage(assistantId, {}, timestamp);
    conversationModel->appendAssistantDelta(assistantId, trimmedContent);
    conversationModel->finishAssistantMessage(
        assistantId, ChatMessageStatus::Complete);
}

void PetWindow::speakPetReply(const QString& text, const QString& source) {
    voiceSynthesis.speak(text, source);
}

void PetWindow::closeEvent(QCloseEvent *event) {
    qDebug() << "PetWindow closing...";
    if (aiBrain) aiBrain->stop();
    hideBubbleMessage();
    launcherChatServer.reset();
    if (outputBubble) outputBubble->close();
    if (inputBubble) inputBubble->close();
    teardownAiRuntime();
    voiceSynthesis.stop();
    unloadModel();
    emit aboutToClose();
    event->accept();
}

void PetWindow::showChatErrorNotification() {
    // Launcher observes failed message metadata and owns its own notification.
    if (launcherChatServer && launcherChatServer->hasAuthenticatedClient()) return;
    if (chatErrorNotification) return;
    auto* notice = new QMessageBox(
        QMessageBox::Warning, QStringLiteral("聊天遇到问题"),
        QStringLiteral("模型服务暂时不可用，请检查网络或模型配置后重试。"),
        QMessageBox::Ok, this);
    chatErrorNotification = notice;
    notice->setAttribute(Qt::WA_DeleteOnClose);
    notice->setWindowModality(Qt::NonModal);
    notice->show();
}

void PetWindow::setupWindow() {
    // 设置窗口属性
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);

    // 设置窗口标志
    updateWindowFlags(alwaysOnTop, clickThrough);
    resize(400, 400);

    // 设置到右下角
    QScreen *screen = QApplication::primaryScreen();
    QRect screenGeometry = screen->availableGeometry();
    int x = screenGeometry.right() - width() - 20;   // 距离右边20像素
    int y = screenGeometry.bottom() - height() - 20; // 距离底部20像素
    move(x, y);

    qDebug() << "PetWindow create with model:" << profile.modelPath;
}

void PetWindow::setupRenderViewport() {
    avatarPresentation = std::make_unique<AvatarPresentation>();
    QFile configuration(ConfigManager::instance().getStateMachinePath());
    if (configuration.open(QIODevice::ReadOnly)) {
        QJsonParseError parse;
        const auto document = QJsonDocument::fromJson(configuration.readAll(), &parse);
        if (parse.error != QJsonParseError::NoError) qWarning() << "[Avatar]" << parse.errorString();
        const auto errors = avatarPresentation->configure(document.object());
        if (!errors.isEmpty()) qWarning() << "[Avatar] invalid configuration:" << errors;
    } else qWarning() << "[Avatar] configuration unavailable:" << configuration.fileName();
    renderViewport = new RenderViewport(this);
    avatarView = renderViewport;
    renderViewport->bindPresentation(avatarPresentation.get());
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        avatarTray = new QSystemTrayIcon(style()->standardIcon(QStyle::SP_ComputerIcon), this);
        auto* menu = new QMenu(this);
        menu->addAction(QStringLiteral("显示形象"), this, [this] { setAvatarVisible(true); });
        menu->addAction(QStringLiteral("隐藏形象"), this, [this] { setAvatarVisible(false); });
        menu->addAction(QStringLiteral("退出"), this, &PetWindow::close);
        avatarTray->setContextMenu(menu); avatarTray->show();
        connect(avatarTray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger) setAvatarVisible(!isVisible());
        });
    }
    renderViewport->setMinimumHeight(400);

    // 设置布局
    auto layout = std::make_unique<QVBoxLayout>(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(renderViewport);

    layout.release();
    
    // 连接渲染视口的初始化完成信号，确保只有在初始化完成后才加载模型
    connect(renderViewport, &RenderViewport::initializationCompleted, this, [this]() {
        qDebug() << "RenderViewport initialization completed, now loading model...";
        if (!renderViewport->loadModel(profile.modelPath)) {
            qWarning() << "Failed to load model:" << profile.modelPath;
        }

    });
}

void PetWindow::updateWindowFlags(bool alwaysOnTop, bool clickThrough) {
    Qt::WindowFlags flags = Qt::FramelessWindowHint;

    if (alwaysOnTop) {
        flags |= Qt::WindowStaysOnTopHint;
    }

    if (clickThrough) {
        flags |= Qt::Tool;
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
    } else {
        setAttribute(Qt::WA_TransparentForMouseEvents, false);
    }

    const bool wasVisible = isVisible();
    changingWindowFlags = true;
    setWindowFlags(flags);
    if (wasVisible) show();
    changingWindowFlags = false;
}

void PetWindow::unloadModel() {
    if (renderViewport) {
        renderViewport->clearModel();
        qDebug() << "Unloading model:" << modelName;
    }
}

void PetWindow::setAvatarVisible(bool visible) {
    if (visible) show(); else hide();
}
void PetWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (!changingWindowFlags && avatarPresentation) avatarPresentation->setVisible(true);
}
void PetWindow::hideEvent(QHideEvent* event) {
    if (!changingWindowFlags && avatarPresentation) {
        avatarPresentation->setVisible(false);
        stopDropAnimation();
        isDragging = false;
    }
    QWidget::hideEvent(event);
}
