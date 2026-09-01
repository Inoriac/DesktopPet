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

#include "render_engine.h"
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
#include "ai/chat/launcher_chat_server.h"
#include "ai/owner/owner_diary_protocol.h"
#include "statistic_manager.h"

namespace {

QString launcherChatStatusName(ChatMessageStatus status) {
    switch (status) {
    case ChatMessageStatus::Pending:
        return QStringLiteral("pending");
    case ChatMessageStatus::Streaming:
        return QStringLiteral("streaming");
    case ChatMessageStatus::Complete:
        return QStringLiteral("complete");
    case ChatMessageStatus::Interrupted:
        return QStringLiteral("interrupted");
    case ChatMessageStatus::Stopped:
        return QStringLiteral("stopped");
    case ChatMessageStatus::Failed:
        return QStringLiteral("failed");
    }
    return QStringLiteral("failed");
}

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
    aiBrain->setEmotionSnapshotProvider([this]() -> std::optional<EmotionSnapshot> {
        if (!emotionEngine || !emotionEngine->isEnabled()) {
            return std::nullopt;
        }
        return emotionEngine->snapshot(QDateTime::currentDateTimeUtc());
    });
    connect(aiBrain.get(), &AIBrain::thinkingStarted, this, [this](const QString& reason) {
        qDebug() << "[AIBrain] thinking started:" << reason;
        startThinkingBubble(reason);
    });
    connect(aiBrain.get(), &AIBrain::thinkingFinished, this, [this](bool success, const QString& errorMessage) {
        qDebug() << "[AIBrain] thinking finished, success:" << success << "error:" << errorMessage;
        const bool wasThinkingBubbleVisible = thinkingBubbleActive;
        if (wasThinkingBubbleVisible && !thinkingHadAssistantResponse) {
            stopThinkingBubble(true);
            const QString fallbackText = success
                ? QStringLiteral("我还没想好怎么说呢。")
                : QStringLiteral("我刚刚有点卡住了，等会儿再试试。") ;
            showBubbleMessage(fallbackText, 3200);
            speakPetReply(fallbackText, QStringLiteral("fallback"));
            return;
        }
        stopThinkingBubble(true);
    });
    connect(aiBrain.get(), &AIBrain::thinkRequestRejected, this,
            [this](const QString& replyToId, const QString& errorMessage) {
                qWarning() << "[AIBrain] chat request could not start:" << errorMessage;
                if (!conversationModel || replyToId.isEmpty()) return;
                const QString assistantId =
                    QUuid::createUuid().toString(QUuid::WithoutBraces);
                conversationModel->beginAssistantMessage(assistantId, replyToId);
                conversationModel->appendAssistantDelta(assistantId, errorMessage);
                conversationModel->finishAssistantMessage(
                    assistantId, ChatMessageStatus::Complete);
                showBubbleMessage(errorMessage, 4000);
            });
    connect(aiBrain.get(), &AIBrain::assistantResponseReady, this, [this](const QString& content) {
        qDebug() << "[AIBrain] assistant response:" << content;
        thinkingHadAssistantResponse = true;
        if (thinkingBubbleActive) {
            stopThinkingBubble(true);
        }
        showBubbleMessageAnimated(content);
        speakPetReply(content, QStringLiteral("assistant"));
    });
    connect(aiBrain.get(), &AIBrain::proactiveResponseReady, this, [this](const QString& content) {
        qDebug() << "[AIBrain] proactive response:" << content;
        if (!thinkingHadAssistantResponse) {
            if (!thinkingBubbleActive) {
                showBubbleMessageAnimated(content);
                speakPetReply(content, QStringLiteral("proactive"));
            }
        }
    });
    connect(aiBrain.get(), &AIBrain::assistantResponseStarted,
            conversationModel.get(),
            [this](const QString& messageId,
                   const QString& replyToId,
                   const QString&) {
                conversationModel->beginAssistantMessage(messageId, replyToId);
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
    if (aiBrain) aiBrain->stopCurrentResponse();
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
    if (emotionBehaviorTimer) {
        emotionBehaviorTimer->stop();
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
    if (typewriterBubbleTimer) {
        typewriterBubbleTimer->stop();
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
        QJsonArray messageArray;
        const QList<ChatHistoryEntry> messages = conversationModel
            ? conversationModel->messages() : QList<ChatHistoryEntry>{};
        constexpr int kMaximumVisibleMessages = 120;
        const int first = std::max(
            0, static_cast<int>(messages.size()) - kMaximumVisibleMessages);
        for (int index = first; index < messages.size(); ++index) {
            const ChatHistoryEntry& entry = messages.at(index);
            messageArray.append(QJsonObject{
                {QStringLiteral("id"), entry.id},
                {QStringLiteral("role"), entry.role},
                {QStringLiteral("replyToId"), entry.replyToId},
                {QStringLiteral("content"), entry.content},
                {QStringLiteral("timestamp"),
                 entry.timestamp.toString(Qt::ISODateWithMs)},
                {QStringLiteral("status"),
                 launcherChatStatusName(entry.status)},
                {QStringLiteral("errorMessage"), entry.errorMessage}
            });
        }
        return QJsonObject{
            {QStringLiteral("petName"), modelName},
            {QStringLiteral("profileId"), profileMigration.profileId},
            {QStringLiteral("aiEnabled"), aiBrain && aiBrain->isEnabled()},
            {QStringLiteral("busy"), aiBrain && aiBrain->isBusy()},
            {QStringLiteral("messages"), messageArray},
            {QStringLiteral("statistics"), launcherChatStatistics(modelName)}
        };
    };
    callbacks.sendMessage = [this](const QString& rawText)
        -> Result<QJsonObject, DomainError> {
        const QString text = rawText.trimmed();
        if (text.isEmpty() || text.size() > 8000) {
            return Result<QJsonObject, DomainError>::failure(domainError(
                QStringLiteral("CHAT_MESSAGE_INVALID"),
                QStringLiteral("消息为空或超过 8000 个字符。")));
        }
        if (!conversationModel || !aiBrain || !aiBrain->isStorageInitialized()) {
            return Result<QJsonObject, DomainError>::failure(domainError(
                QStringLiteral("CHAT_UNAVAILABLE"),
                QStringLiteral("聊天运行时尚未准备好。")));
        }
        if (!aiBrain->isEnabled()) {
            return Result<QJsonObject, DomainError>::failure(domainError(
                QStringLiteral("AI_DISABLED"),
                QStringLiteral("AI 当前没有启用。")));
        }
        if (aiBrain->isBusy()) {
            return Result<QJsonObject, DomainError>::failure(domainError(
                QStringLiteral("CHAT_BUSY"),
                QStringLiteral("上一条消息仍在处理中。")));
        }
        const QString userMessageId = conversationModel->appendUserMessage(text);
        if (userMessageId.isEmpty()) {
            return Result<QJsonObject, DomainError>::failure(domainError(
                QStringLiteral("CHAT_MESSAGE_INVALID"),
                QStringLiteral("消息无法加入聊天记录。")));
        }
        if (petController) petController->recordExplicitFeedbackText(text);
        aiBrain->triggerThink(text, QStringLiteral("user_request"), userMessageId);
        return Result<QJsonObject, DomainError>::success({
            {QStringLiteral("messageId"), userMessageId}
        });
    };
    callbacks.retryMessage = [this](const QString& assistantMessageId)
        -> Result<QJsonObject, DomainError> {
        if (!conversationModel || !aiBrain || !aiBrain->isEnabled()
            || aiBrain->isBusy()) {
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
        const auto source = std::find_if(
            messages.cbegin(), messages.cend(),
            [&assistant](const ChatHistoryEntry& entry) {
                return entry.id == assistant->replyToId
                    && entry.role == QLatin1String("user");
            });
        if (source == messages.cend()) {
            return Result<QJsonObject, DomainError>::failure(domainError(
                QStringLiteral("CHAT_MESSAGE_NOT_FOUND"),
                QStringLiteral("找不到原始用户消息。")));
        }
        const QString text = source->content;
        const QString newUserId = conversationModel->appendUserMessage(text);
        if (newUserId.isEmpty()) {
            return Result<QJsonObject, DomainError>::failure(domainError(
                QStringLiteral("CHAT_MESSAGE_INVALID"),
                QStringLiteral("消息无法加入聊天记录。")));
        }
        if (petController) petController->recordExplicitFeedbackText(text);
        aiBrain->triggerThink(text, QStringLiteral("user_request"), newUserId);
        return Result<QJsonObject, DomainError>::success({
            {QStringLiteral("messageId"), newUserId}
        });
    };
    callbacks.stopResponse = [this]() -> Result<void, DomainError> {
        if (aiBrain && aiBrain->isBusy()) aiBrain->stopCurrentResponse();
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

void PetWindow::openChatHistoryWindow() {
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
    if (aiBrain) aiBrain->stopCurrentResponse();
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
    renderViewport = new RenderViewport(this);
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

        setupAiAnimationTools();
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

    setWindowFlags(flags);

    // 强制更新窗口
    if (isVisible()) {
        hide();
        show();
    }
}

bool PetWindow::loadModel(const QString &modelPath) {
    if (!renderViewport) {
        qWarning() << "RenderViewport not initialized";
        return false;
    }
    
    return renderViewport->loadModel(modelPath);
}

void PetWindow::unloadModel() {
    if (renderViewport) {
        renderViewport->clearModel();
        qDebug() << "Unloading model:" << modelName;
    }
}

void PetWindow::playAnimation() {
    // 动画播放控制逻辑
    qDebug() << "Playing animation";
}

void PetWindow::pauseAnimation() {
    // 动画暂停控制逻辑
    qDebug() << "Pausing animation";
}

void PetWindow::stopAnimation() {
    // 动画停止控制逻辑
    qDebug() << "Stopping animation";
}

void PetWindow::setAnimationSpeed(float speed) {
    // 设置动画速度逻辑
    qDebug() << "Setting animation speed:" << speed;
}

void PetWindow::setAnimationLoop(bool loop) {
    // 设置动画循环逻辑
    qDebug() << "Setting animation loop:" << loop;
}
