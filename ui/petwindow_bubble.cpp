//
// Created by Inoriac on 2025/10/15.
//

#include "petwindow.h"

#include "liquidglasschatbubble.h"

#include <QGuiApplication>
#include <QRandomGenerator>
#include <QScreen>
#include <QStringList>
#include <QTimer>

#include <algorithm>

namespace {
constexpr int kBubblePageMaxChars = 200;

int readableBreakIndex(const QString& text, int maxChars) {
    const QString head = text.left(std::max(0, maxChars));
    const QString breakChars = QStringLiteral("。！？!?；;，,\n");
    const int headSize = static_cast<int>(head.size());
    for (int i = headSize - 1; i >= std::max(0, headSize - 45); --i) {
        if (breakChars.contains(head.at(i))) {
            return i + 1;
        }
    }
    return std::min(maxChars, headSize);
}
}

void PetWindow::showBubbleMessage(const QString& message, int durationMs) {
    stopTypewriterBubble();
    bubblePages = splitBubbleTextIntoPages(message);
    bubblePageIndex = 0;
    showCurrentBubblePageAnimated(durationMs);
}

void PetWindow::showBubbleMessageNow(const QString& message, int durationMs, bool forceRefreshGlass) {
    if (!outputBubble) {
        return;
    }

    outputBubble->applyScreenChatConfig(screenChatConfig);
    const QSize oldSize = outputBubble->size();
    const bool wasVisible = outputBubble->isVisible();
    outputBubble->setHasMorePages(hasMoreBubblePages() && !typewriterBubbleActive);
    outputBubble->setMessage(message);
    updateOutputBubblePosition();
    if (forceRefreshGlass || !wasVisible || oldSize != outputBubble->size()) {
        outputBubble->refreshGlass();
    }
    outputBubble->show();
    outputBubble->raise();

    if (bubbleHideTimer) {
        const int resolvedDuration = durationMs > 0 ? durationMs : screenChatConfig.bubbleDurationMs;
        bubbleHideTimer->start(std::max(1000, resolvedDuration));
    }
}

void PetWindow::showBubbleMessageAnimated(const QString& message, int durationMs) {
    if (!outputBubble) {
        return;
    }

    stopTypewriterBubble();
    bubblePages = splitBubbleTextIntoPages(message);
    bubblePageIndex = 0;
    showCurrentBubblePageAnimated(durationMs);
}

void PetWindow::showCurrentBubblePageAnimated(int durationMs) {
    if (!outputBubble) {
        return;
    }

    stopTypewriterBubble();
    if (bubblePages.isEmpty()) {
        bubblePages = QStringList{QStringLiteral("...")};
        bubblePageIndex = 0;
    }
    bubblePageIndex = std::clamp(bubblePageIndex, 0, static_cast<int>(bubblePages.size()) - 1);
    typewriterTargetText = bubblePages.at(bubblePageIndex);
    typewriterVisibleChars = 0;
    typewriterFinalDurationMs = durationMs;
    typewriterBubbleActive = true;
    outputBubble->setHasMorePages(false);

    if (!typewriterBubbleTimer) {
        typewriterBubbleTimer = new QTimer(this);
        connect(typewriterBubbleTimer, &QTimer::timeout, this, &PetWindow::updateTypewriterBubble);
    }
    if (bubbleHideTimer) {
        bubbleHideTimer->stop();
    }

    updateTypewriterBubble();
    typewriterBubbleTimer->start(32);
}

bool PetWindow::hasMoreBubblePages() const {
    return bubblePageIndex >= 0 && bubblePageIndex + 1 < bubblePages.size();
}

void PetWindow::showNextBubblePage() {
    if (!hasMoreBubblePages()) {
        return;
    }

    ++bubblePageIndex;
    showCurrentBubblePageAnimated(typewriterFinalDurationMs);
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
    if (outputBubble) {
        outputBubble->setHasMorePages(false);
        outputBubble->hideBubble();
    }
}

void PetWindow::startThinkingBubble(const QString& reason) {
    thinkingHadAssistantResponse = false;
    stopTypewriterBubble();
    bubblePages.clear();
    bubblePageIndex = 0;
    if (outputBubble) {
        outputBubble->setHasMorePages(false);
    }

    const QString normalizedReason = reason.trimmed();
    if (normalizedReason == QStringLiteral("idle_tick")
        || normalizedReason == QStringLiteral("emotion_tick")
        || normalizedReason == QStringLiteral("proactive_chat_tick")
        || normalizedReason == QStringLiteral("busy_retry")) {
        return;
    }

    if (!outputBubble) {
        return;
    }

    static const QStringList phrases = {
        QStringLiteral("让我想想"),
        QStringLiteral("脑袋转转"),
        QStringLiteral("正在想办法")
    };

    thinkingBubbleActive = true;
    thinkingDotCount = 1;
    thinkingBubbleTextBase = phrases.at(QRandomGenerator::global()->bounded(phrases.size()));

    if (!thinkingBubbleTimer) {
        thinkingBubbleTimer = new QTimer(this);
        connect(thinkingBubbleTimer, &QTimer::timeout, this, &PetWindow::updateThinkingBubble);
    }
    if (bubbleHideTimer) {
        bubbleHideTimer->stop();
    }

    updateThinkingBubble();
    thinkingBubbleTimer->start(380);
}

void PetWindow::stopThinkingBubble(bool keepCurrentBubble) {
    if (thinkingBubbleTimer) {
        thinkingBubbleTimer->stop();
    }

    if (!thinkingBubbleActive) {
        return;
    }

    thinkingBubbleActive = false;
    thinkingDotCount = 1;
    thinkingBubbleTextBase.clear();

    if (!keepCurrentBubble) {
        if (bubbleHideTimer) {
            bubbleHideTimer->stop();
        }
        hideBubbleMessage();
    }
}

void PetWindow::updateThinkingBubble() {
    if (!thinkingBubbleActive || thinkingBubbleTextBase.isEmpty()) {
        return;
    }

    const QString dots(thinkingDotCount, QLatin1Char('.'));
    showBubbleMessageNow(QStringLiteral("%1%2").arg(thinkingBubbleTextBase, dots), 1600, false);
    thinkingDotCount = thinkingDotCount >= 6 ? 1 : thinkingDotCount + 1;
}

void PetWindow::stopTypewriterBubble() {
    if (typewriterBubbleTimer) {
        typewriterBubbleTimer->stop();
    }
    typewriterBubbleActive = false;
    typewriterTargetText.clear();
    typewriterVisibleChars = 0;
    typewriterFinalDurationMs = -1;
}

void PetWindow::updateTypewriterBubble() {
    if (!typewriterBubbleActive) {
        return;
    }

    if (typewriterTargetText.isEmpty()) {
        stopTypewriterBubble();
        showBubbleMessageNow(QStringLiteral("嗯……我刚刚没组织好语言。"), 3200);
        return;
    }

    const int remaining = typewriterTargetText.size() - typewriterVisibleChars;
    const int step = 1;
    typewriterVisibleChars += std::min(step, std::max(0, remaining));

    const QString visibleText = typewriterTargetText.left(typewriterVisibleChars);
    const bool finished = typewriterVisibleChars >= typewriterTargetText.size();
    const bool needsGlassRefresh = !outputBubble || !outputBubble->isVisible() || typewriterVisibleChars <= step;
    showBubbleMessageNow(visibleText, finished ? typewriterFinalDurationMs : 1600, needsGlassRefresh);

    if (finished) {
        stopTypewriterBubble();
        if (outputBubble) {
            const bool hasMore = hasMoreBubblePages();
            outputBubble->setHasMorePages(hasMore);
            updateOutputBubblePosition();
            outputBubble->refreshGlass();
            if (hasMore && bubbleHideTimer) {
                bubbleHideTimer->stop();
            }
        }
    }
}

QStringList PetWindow::splitBubbleTextIntoPages(const QString& message) const {
    const QString normalized = message.trimmed();
    if (normalized.isEmpty()) {
        return {QStringLiteral("...")};
    }

    QStringList pages;
    QString remaining = normalized;
    while (!remaining.isEmpty()) {
        if (remaining.size() <= kBubblePageMaxChars) {
            pages.append(remaining.trimmed());
            break;
        }

        int splitAt = readableBreakIndex(remaining, kBubblePageMaxChars);
        if (splitAt <= 0) {
            splitAt = std::min(kBubblePageMaxChars, static_cast<int>(remaining.size()));
        }

        pages.append(remaining.left(splitAt).trimmed());
        remaining = remaining.mid(splitAt).trimmed();
    }

    return pages.isEmpty() ? QStringList{QStringLiteral("...")} : pages;
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
}

void PetWindow::updateInputBubblePosition() {
    if (!inputBubble) {
        return;
    }

    const QRect rect = frameGeometry();
    const QSize bubbleSize = inputBubble->sizeHint();
    int targetX = rect.left() + (rect.width() - bubbleSize.width()) / 2 + screenChatConfig.bubbleOffsetX;
    int targetY = rect.bottom() + 8;
    inputBubble->resize(bubbleSize);
    inputBubble->move(targetX, targetY);
}
