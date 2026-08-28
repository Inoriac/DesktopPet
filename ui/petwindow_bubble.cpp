//
// Created by Inoriac on 2025/10/15.
//

#include "petwindow.h"

#include "bubble_playback_controller.h"
#include "liquidglasschatbubble.h"
#include "streaming_text_paginator.h"
#include "thinking_status_selector.h"

#include <QGuiApplication>
#include <QScreen>
#include <QStringList>
#include <QTimer>

#include <algorithm>

void PetWindow::showBubbleMessage(const QString& message, int durationMs) {
    if (!outputBubble || !streamingTextPaginator
        || !bubblePlaybackController) {
        return;
    }
    if (!streamingBubbleMessageId.isEmpty() && !streamingBubbleFinished) {
        return;
    }
    stopTypewriterBubble();
    if (thinkingBubbleTimer) thinkingBubbleTimer->stop();
    if (bubbleHideTimer) bubbleHideTimer->stop();
    streamingBubbleMessageId.clear();
    streamingBubbleHasText = !message.isEmpty();
    streamingBubbleFinished = false;
    streamingBubbleHideDurationMs = durationMs;
    streamingTextPaginator->reset();
    bubblePlaybackController->reset({});
    outputBubble->applyScreenChatConfig(screenChatConfig);
    outputBubble->showStreamingMessage({});
    outputBubble->setActivityText({});
    applyBubblePaginationUpdate(streamingTextPaginator->feed(message));
    applyBubblePaginationUpdate(streamingTextPaginator->finish());
    streamingBubbleFinished = true;
    if (bubblePlaybackController->pageCount() == 0) {
        outputBubble->setActivityText(QStringLiteral("暂时没有内容"));
    }
    updateOutputBubblePosition();
    scheduleFinishedBubbleHide();
}

void PetWindow::showBubbleMessageNow(const QString& message, int durationMs, bool forceRefreshGlass) {
    Q_UNUSED(forceRefreshGlass)
    showBubbleMessage(message, durationMs);
}

void PetWindow::showBubbleMessageAnimated(const QString& message, int durationMs) {
    if (!streamingBubbleMessageId.isEmpty() && !streamingBubbleFinished) {
        return;
    }
    showBubbleMessage(message, durationMs);
}

void PetWindow::showCurrentBubblePageAnimated(int durationMs) {
    if (bubblePages.isEmpty()) return;
    bubblePageIndex = std::clamp(
        bubblePageIndex, 0, static_cast<int>(bubblePages.size()) - 1);
    showBubbleMessage(bubblePages.at(bubblePageIndex), durationMs);
}

bool PetWindow::hasMoreBubblePages() const {
    return bubblePlaybackController
        && bubblePlaybackController->hasUnreadPages();
}

void PetWindow::showNextBubblePage() {
    if (bubblePlaybackController) bubblePlaybackController->next();
}

void PetWindow::showBubbleInput() {
    if (!inputBubble) {
        return;
    }

    inputBubble->applyScreenChatConfig(screenChatConfig);
    updateInputBubblePosition();
    inputBubble->refreshGlass();
    inputBubble->showInput("输入后按 Enter 发送...");
}

void PetWindow::hideBubbleMessage() {
    stopTypewriterBubble();
    if (!streamingBubbleFinished
        || (bubblePlaybackController
            && bubblePlaybackController->hasUnreadPages())) {
        return;
    }
    if (outputBubble) {
        outputBubble->setHasMorePages(false);
        outputBubble->hideBubble();
    }
}

void PetWindow::startThinkingBubble(const QString& reason) {
    Q_UNUSED(reason)
    // assistantResponseStarted immediately follows this legacy signal and owns
    // the visible waiting state. Suppress the legacy fallback bubble.
    thinkingHadAssistantResponse = true;
    thinkingBubbleActive = true;
    stopTypewriterBubble();
    if (bubbleHideTimer) bubbleHideTimer->stop();
}

void PetWindow::stopThinkingBubble(bool keepCurrentBubble) {
    if (thinkingBubbleTimer) {
        thinkingBubbleTimer->stop();
    }

    thinkingBubbleActive = false;
    thinkingDotCount = 1;
    thinkingBubbleTextBase.clear();

    if (!keepCurrentBubble) {
        if (bubbleHideTimer) bubbleHideTimer->stop();
        if (outputBubble) outputBubble->hideBubble();
    }
}

void PetWindow::updateThinkingBubble() {
    if (streamingBubbleMessageId.isEmpty() || !thinkingStatusSelector
        || !outputBubble) {
        return;
    }
    outputBubble->setActivityText(thinkingStatusSelector->next(
        streamingBubbleStage, streamingBubbleMessageId));
    updateOutputBubblePosition();
}

void PetWindow::beginStreamingBubble(const QString& messageId) {
    if (messageId.isEmpty() || !outputBubble || !streamingTextPaginator
        || !bubblePlaybackController || !thinkingStatusSelector) {
        return;
    }
    if (bubbleHideTimer) bubbleHideTimer->stop();
    stopTypewriterBubble();
    streamingBubbleMessageId = messageId;
    streamingBubbleHasText = false;
    streamingBubbleFinished = false;
    streamingBubbleHideDurationMs = -1;
    streamingBubbleStage = ChatActivityStage::WaitingForModel;
    thinkingBubbleActive = true;
    thinkingHadAssistantResponse = true;
    streamingTextPaginator->reset();
    bubblePlaybackController->reset(messageId);
    thinkingStatusSelector->reset(messageId);
    outputBubble->applyScreenChatConfig(screenChatConfig);
    outputBubble->showStreamingMessage(messageId);
    if (inputBubble) inputBubble->setInputSubmissionEnabled(false);
    if (!thinkingBubbleTimer) {
        thinkingBubbleTimer = new QTimer(this);
        connect(thinkingBubbleTimer, &QTimer::timeout,
                this, &PetWindow::updateThinkingBubble);
    }
    updateThinkingBubble();
    thinkingBubbleTimer->start(2000);
    updateOutputBubblePosition();
}

void PetWindow::updateStreamingBubbleStage(const QString& messageId,
                                           ChatActivityStage stage) {
    if (messageId != streamingBubbleMessageId || streamingBubbleFinished) return;
    streamingBubbleStage = stage;
    if (streamingBubbleHasText && stage == ChatActivityStage::StreamingText) {
        if (thinkingBubbleTimer) thinkingBubbleTimer->stop();
        if (outputBubble) outputBubble->setActivityText({});
        return;
    }
    updateThinkingBubble();
    if (thinkingBubbleTimer) thinkingBubbleTimer->start(2000);
}

void PetWindow::appendStreamingBubbleDelta(const QString& messageId,
                                           const QString& delta) {
    if (messageId != streamingBubbleMessageId || streamingBubbleFinished
        || delta.isEmpty() || !streamingTextPaginator) {
        return;
    }
    streamingBubbleHasText = true;
    thinkingHadAssistantResponse = true;
    if (streamingBubbleStage == ChatActivityStage::StreamingText) {
        if (thinkingBubbleTimer) thinkingBubbleTimer->stop();
        if (outputBubble) outputBubble->setActivityText({});
    }
    applyBubblePaginationUpdate(streamingTextPaginator->feed(delta));
}

void PetWindow::finishStreamingBubble(const QString& messageId,
                                      ChatMessageStatus status) {
    if (messageId != streamingBubbleMessageId || streamingBubbleFinished
        || !streamingTextPaginator || !bubblePlaybackController) {
        return;
    }
    if (thinkingBubbleTimer) thinkingBubbleTimer->stop();
    applyBubblePaginationUpdate(streamingTextPaginator->finish());
    streamingBubbleFinished = true;
    thinkingBubbleActive = false;
    if (inputBubble) inputBubble->setInputSubmissionEnabled(true);

    if (outputBubble) {
        if (bubblePlaybackController->pageCount() == 0) {
            switch (status) {
            case ChatMessageStatus::Stopped:
                outputBubble->setActivityText(QStringLiteral("已停止回复"));
                break;
            case ChatMessageStatus::Interrupted:
                outputBubble->setActivityText(QStringLiteral("回复中断"));
                break;
            case ChatMessageStatus::Failed:
                outputBubble->setActivityText(QStringLiteral("这次没有顺利完成"));
                break;
            default:
                outputBubble->setActivityText(QStringLiteral("暂时没有可展示的回复"));
                break;
            }
        } else if (status == ChatMessageStatus::Complete) {
            outputBubble->setActivityText({});
        } else if (status == ChatMessageStatus::Stopped) {
            outputBubble->setActivityText(QStringLiteral("已停止"));
        } else {
            outputBubble->setActivityText(QStringLiteral("回复中断"));
        }
        updateOutputBubblePosition();
    }
    scheduleFinishedBubbleHide();
}

void PetWindow::applyBubblePaginationUpdate(const PaginationUpdate& update) {
    if (!bubblePlaybackController) return;
    if (!update.newlySealedPages.isEmpty()) {
        bubblePlaybackController->appendSealedPages(update.newlySealedPages);
    }
    bubblePlaybackController->updateDraftPage(update.draftPage);
}

void PetWindow::scheduleFinishedBubbleHide() {
    if (!streamingBubbleFinished || !bubbleHideTimer
        || !bubblePlaybackController
        || bubblePlaybackController->isHovered()
        || bubblePlaybackController->hasUnreadPages()) {
        if (bubbleHideTimer) bubbleHideTimer->stop();
        return;
    }
    const int duration = streamingBubbleHideDurationMs > 0
        ? streamingBubbleHideDurationMs
        : screenChatConfig.bubbleDurationMs;
    bubbleHideTimer->start(std::max(1000, duration));
}

void PetWindow::stopTypewriterBubble() {
    if (typewriterBubbleTimer) {
        typewriterBubbleTimer->stop();
    }
    typewriterBubbleActive = false;
    typewriterTargetText.clear();
    typewriterVisibleChars = 0;
    typewriterFinalDurationMs = -1;
    if (outputBubble) {
        outputBubble->setLayoutReserveText(QString());
    }
}

void PetWindow::updateTypewriterBubble() {
    stopTypewriterBubble();
}

QStringList PetWindow::splitBubbleTextIntoPages(const QString& message) const {
    StreamingTextPaginator paginator;
    const PaginationUpdate streamed = paginator.feed(message);
    const PaginationUpdate finished = paginator.finish();
    return streamed.newlySealedPages + finished.newlySealedPages;
}

void PetWindow::updateBubblePositions() {
    updateOutputBubblePosition();
    updateInputBubblePosition();
}

void PetWindow::updateOutputBubblePosition() {
    if (!outputBubble) {
        return;
    }

    const QRect rect = frameGeometry();
    const QSize bubbleSize = outputBubble->sizeHint();
    int targetX = rect.left() + (rect.width() - bubbleSize.width()) / 2 + screenChatConfig.bubbleOffsetX;
    int targetY = rect.top() - bubbleSize.height() + screenChatConfig.bubbleOffsetY;
    QScreen* screen = QGuiApplication::screenAt(rect.center());
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen) {
        const QRect available = screen->availableGeometry().adjusted(8, 8, -8, -8);
        const int maxX = std::max(available.left(), available.right() - bubbleSize.width() + 1);
        const int maxY = std::max(available.top(), available.bottom() - bubbleSize.height() + 1);
        targetX = std::clamp(targetX, available.left(), maxX);
        targetY = std::clamp(targetY, available.top(), maxY);
    }
    outputBubble->resize(bubbleSize);
    outputBubble->move(targetX, targetY);
    outputBubble->scheduleDynamicRefresh(false);
}

void PetWindow::updateInputBubblePosition() {
    if (!inputBubble) {
        return;
    }

    const QRect rect = frameGeometry();
    const QSize bubbleSize = inputBubble->sizeHint();
    int targetX = rect.left() + (rect.width() - bubbleSize.width()) / 2 + screenChatConfig.bubbleOffsetX;
    int targetY = rect.bottom() + 8;
    QScreen* screen = QGuiApplication::screenAt(rect.center());
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect available = screen->availableGeometry().adjusted(8, 8, -8, -8);
        const int maxX = std::max(
            available.left(), available.right() - bubbleSize.width() + 1);
        const int maxY = std::max(
            available.top(), available.bottom() - bubbleSize.height() + 1);
        targetX = std::clamp(targetX, available.left(), maxX);
        targetY = std::clamp(targetY, available.top(), maxY);
    }
    inputBubble->resize(bubbleSize);
    inputBubble->move(targetX, targetY);
    inputBubble->scheduleDynamicRefresh(false);
}
