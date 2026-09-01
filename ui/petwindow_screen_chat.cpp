//
// Created by Inoriac on 2025/10/15.
//

#include "petwindow.h"

#include "configLoader/config_manager.h"
#include "controller/pet_controller.h"
#include "bubble_playback_controller.h"
#include "chat_conversation_model.h"
#include "liquidglasschatbubble.h"
#include "streaming_text_paginator.h"
#include "thinking_status_selector.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QScreen>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include <algorithm>

namespace {
QString extractJsonPayload(const QString& text) {
    const QString trimmed = text.trimmed();
    if (trimmed.startsWith('{') && trimmed.endsWith('}')) {
        return trimmed;
    }

    QRegularExpression fenced(R"(```(?:json)?\s*(\{[\s\S]*\})\s*```)");
    QRegularExpressionMatch match = fenced.match(text);
    if (match.hasMatch()) {
        return match.captured(1);
    }

    const int start = text.indexOf('{');
    const int end = text.lastIndexOf('}');
    if (start >= 0 && end > start) {
        return text.mid(start, end - start + 1);
    }
    return {};
}
}

void PetWindow::setupScreenChat() {
    screenChatTimer = new QTimer(this);
    screenChatTimer->setSingleShot(true);
    connect(screenChatTimer, &QTimer::timeout, this, [this]() {
        triggerScreenChat(false, "timer");
    });

    bubbleHideTimer = new QTimer(this);
    bubbleHideTimer->setSingleShot(true);
    connect(bubbleHideTimer, &QTimer::timeout, this, &PetWindow::hideBubbleMessage);

    outputBubble = new LiquidGlassChatBubble(nullptr);
    outputBubble->applyScreenChatConfig(screenChatConfig);
    outputBubble->hide();
    connect(outputBubble, &LiquidGlassChatBubble::morePagesRequested, this, &PetWindow::showNextBubblePage);

    streamingTextPaginator = std::make_unique<StreamingTextPaginator>();
    bubblePlaybackController = std::make_unique<BubblePlaybackController>(this);
    thinkingStatusSelector = std::make_unique<ThinkingStatusSelector>();
    connect(outputBubble, &LiquidGlassChatBubble::previousPageRequested,
            bubblePlaybackController.get(),
            &BubblePlaybackController::previous);
    connect(outputBubble, &LiquidGlassChatBubble::nextPageRequested,
            bubblePlaybackController.get(), &BubblePlaybackController::next);
    connect(outputBubble, &LiquidGlassChatBubble::playbackToggleRequested,
            bubblePlaybackController.get(),
            &BubblePlaybackController::toggleUserPause);
    connect(outputBubble, &LiquidGlassChatBubble::hoveredChanged,
            bubblePlaybackController.get(),
            &BubblePlaybackController::setHovered);
    connect(outputBubble, &LiquidGlassChatBubble::hoveredChanged,
            this, [this](bool hovered) {
                if (hovered) {
                    if (bubbleHideTimer) bubbleHideTimer->stop();
                } else {
                    scheduleFinishedBubbleHide();
                }
            });
    connect(outputBubble, &LiquidGlassChatBubble::openConversationRequested,
            this, [this](const QString& messageId) {
                Q_UNUSED(messageId)
                openChatHistoryWindow();
            });
    connect(bubblePlaybackController.get(),
            &BubblePlaybackController::pageChanged,
            this,
            [this](const QString& text, int index, int total, bool draft) {
                pendingBubblePageMessageId = bubblePlaybackController
                    ? bubblePlaybackController->messageId() : QString();
                pendingBubblePageText = text;
                pendingBubblePageIndex = index;
                pendingBubblePageTotal = total;
                pendingBubblePageDraft = draft;
                if (bubblePageFlushPending) return;
                bubblePageFlushPending = true;
                QTimer::singleShot(0, this, [this]() {
                    bubblePageFlushPending = false;
                    if (!outputBubble || !bubblePlaybackController
                        || pendingBubblePageMessageId
                            != bubblePlaybackController->messageId()) {
                        return;
                    }
                    outputBubble->setDisplayedPage(
                        pendingBubblePageText, pendingBubblePageIndex,
                        pendingBubblePageTotal, pendingBubblePageDraft);
                    updateOutputBubblePosition();
                    if (streamingBubbleFinished) {
                        scheduleFinishedBubbleHide();
                    }
                });
            });
    connect(bubblePlaybackController.get(),
            &BubblePlaybackController::playbackStateChanged,
            outputBubble, &LiquidGlassChatBubble::setPlaybackPaused);

    inputBubble = new LiquidGlassChatBubble(nullptr);
    inputBubble->applyScreenChatConfig(screenChatConfig);
    inputBubble->setInputAutoFadeEnabled(true);
    inputBubble->showInput("输入后按 Enter 发送...", false);
    updateInputBubblePosition();
    inputBubble->refreshGlass();
    connect(inputBubble, &LiquidGlassChatBubble::messageSubmitted, this, [this](const QString& text) {
        if (!conversationModel) return;
        const QString userMessageId = conversationModel->appendUserMessage(text);
        if (userMessageId.isEmpty()) return;
        if (petController) {
            petController->recordExplicitFeedbackText(text);
        }
        if (!aiBrain || !aiBrain->isEnabled()) {
            qWarning() << "[AIBrain] user input ignored, AI disabled";
            const QString assistantId =
                QUuid::createUuid().toString(QUuid::WithoutBraces);
            conversationModel->beginAssistantMessage(
                assistantId, userMessageId);
            conversationModel->appendAssistantDelta(
                assistantId,
                QStringLiteral("AI 当前没有启用，暂时不能回复。"));
            conversationModel->finishAssistantMessage(
                assistantId, ChatMessageStatus::Complete);
            showBubbleMessage(
                QStringLiteral("AI 当前没有启用，暂时不能回复。"));
            return;
        }
        aiBrain->triggerThink(text, "user_request", userMessageId);
    });

    // setupScreenChat runs before the model and AIBrain are constructed.
    // Bind their UI lifecycle once the constructor returns to the event loop.
    QTimer::singleShot(0, this, [this]() {
        if (!aiBrain || !conversationModel) return;
        connect(aiBrain.get(), &AIBrain::assistantResponseStarted,
                this,
                [this](const QString& messageId,
                       const QString&,
                       const QString&) {
                    beginStreamingBubble(messageId);
                });
        connect(aiBrain.get(), &AIBrain::assistantResponseStageChanged,
                this, &PetWindow::updateStreamingBubbleStage);
        connect(aiBrain.get(), &AIBrain::assistantResponseDelta,
                this, &PetWindow::appendStreamingBubbleDelta);
        connect(aiBrain.get(), &AIBrain::assistantResponseFinished,
                this,
                [this](const QString& messageId,
                       ChatMessageStatus status,
                       const QString&) {
                    finishStreamingBubble(messageId, status);
                });
    });

#ifdef Q_OS_WIN
    outputBubble->winId();
    inputBubble->winId();
#endif
}

void PetWindow::updateScreenChatSchedule() {
    if (!screenChatTimer) {
        return;
    }

    if (!screenChatConfig.enabled) {
        screenChatTimer->stop();
        return;
    }

    scheduleNextScreenChat();
}

void PetWindow::scheduleNextScreenChat() {
    if (!screenChatTimer || !screenChatConfig.enabled) {
        return;
    }

    const int minMs = std::max(1000, screenChatConfig.minIntervalMs);
    const int maxMs = std::max(minMs, screenChatConfig.maxIntervalMs);
    const int nextMs = QRandomGenerator::global()->bounded(minMs, maxMs + 1);
    screenChatTimer->start(nextMs);
    qDebug() << "[ScreenChat] next trigger in ms:" << nextMs;
}

void PetWindow::triggerScreenChatNow(const QString& reason) {
    triggerScreenChat(false, reason);
}

QString PetWindow::captureDesktopScreenshot(bool debugKeepCopy, QString* debugCopyPath) const {
    QScreen* screen = QGuiApplication::primaryScreen();
    if (!screen) {
        qWarning() << "[ScreenChat] primary screen not found";
        return {};
    }

    const QPixmap shot = screen->grabWindow(0);
    if (shot.isNull()) {
        qWarning() << "[ScreenChat] grabWindow failed";
        return {};
    }

    const QString fileName = QString("desktop_pet_capture_%1.png").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString tempPath = QDir::temp().absoluteFilePath(fileName);
    if (!shot.save(tempPath, "PNG")) {
        qWarning() << "[ScreenChat] failed to save temp screenshot:" << tempPath;
        return {};
    }

    if (debugKeepCopy && debugCopyPath) {
        QDir logDir("log");
        if (!logDir.exists()) {
            logDir.mkpath(".");
        }
        const QString debugPath = logDir.absoluteFilePath(fileName);
        if (QFile::copy(tempPath, debugPath)) {
            *debugCopyPath = debugPath;
        }
    }

    return tempPath;
}

void PetWindow::triggerScreenChat(bool debugSaveScreenshotOnly, const QString& reason) {
    if (screenChatBusy) {
        qDebug() << "[ScreenChat] skip, request already in-flight";
        if (screenChatConfig.enabled) {
            scheduleNextScreenChat();
        }
        return;
    }

    QString debugCopyPath;
    const QString screenshotPath = captureDesktopScreenshot(debugSaveScreenshotOnly, &debugCopyPath);
    if (screenshotPath.isEmpty()) {
        if (screenChatConfig.enabled) {
            scheduleNextScreenChat();
        }
        return;
    }

    if (debugSaveScreenshotOnly) {
        QFile::remove(screenshotPath);
        const QString message = debugCopyPath.isEmpty()
            ? QString("截图已完成，但保存到log失败")
            : QString("调试截图已保存: %1").arg(debugCopyPath);
        showBubbleMessage(message);
        return;
    }

    requestVisionSummary(screenshotPath, reason, false);
}

void PetWindow::requestVisionSummary(const QString& screenshotPath,
                                     const QString& reason,
                                     bool debugSaveScreenshotOnly) {
    Q_UNUSED(debugSaveScreenshotOnly);

    if (!aiBrain || !aiBrain->isEnabled()) {
        qWarning() << "[ScreenChat] skipped, LLM disabled";
        QFile::remove(screenshotPath);
        if (screenChatConfig.enabled) {
            scheduleNextScreenChat();
        }
        return;
    }

    QFile imageFile(screenshotPath);
    if (!imageFile.open(QIODevice::ReadOnly)) {
        qWarning() << "[ScreenChat] failed to open screenshot" << screenshotPath;
        QFile::remove(screenshotPath);
        if (screenChatConfig.enabled) {
            scheduleNextScreenChat();
        }
        return;
    }

    const QByteArray imageBytes = imageFile.readAll();
    imageFile.close();

    const QString styleHint = QString("请根据宠物性别(%1)生成偏日常、自然口吻的一句话，不要过度夸张。")
        .arg(screenChatConfig.petGender);

    const QString prompt = QString(
        "你是桌宠视觉助手。请识别图片主要内容，并输出JSON，格式严格为"
        " {\"main_content\":\"...\",\"pet_reply\":\"...\"}。"
        "要求：main_content不超过20字；pet_reply不超过24字；仅输出JSON，无其它文字。%1")
        .arg(styleHint);
    ChatMessage message;
    message.role = QStringLiteral("user");
    message.content = prompt;
    message.contentBlocks = QJsonArray{
        QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                    {QStringLiteral("text"), prompt}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("image")},
                    {QStringLiteral("mediaType"), QStringLiteral("image/png")},
                    {QStringLiteral("data"),
                     QString::fromLatin1(imageBytes.toBase64())}}
    };
    ModelRequest request;
    request.role = ModelRole::Vision;
    request.constraints.requiresVision = true;
    request.messages = {message};
    request.petName = modelName;

    screenChatBusy = true;
    QPointer<PetWindow> guard(this);
    aiBrain->modelRouter()->completeAsync(
        request,
        [guard, screenshotPath, reason](
            Result<ModelCompletion, DomainError> result) {
        if (!guard) {
            QFile::remove(screenshotPath);
            return;
        }
        QString bubbleText;
        if (!result.isOk()) {
            qWarning() << "[ScreenChat] vision route failed"
                       << result.error().code << result.error().message;
            bubbleText = "刚刚看了一眼屏幕，网络有点忙呢";
        } else {
            const QString jsonPayload = extractJsonPayload(
                result.value().response.content);
            const QJsonDocument resultDoc = QJsonDocument::fromJson(
                jsonPayload.toUtf8());
            if (resultDoc.isObject()) {
                const QJsonObject resultObj = resultDoc.object();
                bubbleText = resultObj.value("pet_reply").toString().trimmed();
                const QString mainContent =
                    resultObj.value("main_content").toString().trimmed();
                qDebug() << "[ScreenChat] reason=" << reason
                         << "main_content=" << mainContent
                         << "pet_reply=" << bubbleText;
            }
        }

        if (bubbleText.isEmpty()) {
            bubbleText = "我看到你在忙，要记得休息呀";
        }

        guard->showBubbleMessage(bubbleText);
        guard->speakPetReply(bubbleText, QStringLiteral("screenChat"));
        QFile::remove(screenshotPath);
        guard->screenChatBusy = false;
        if (guard->screenChatConfig.enabled) {
            guard->scheduleNextScreenChat();
        }
    });
}
