#include "bubble_playback_controller.h"

#include <algorithm>

BubblePlaybackController::BubblePlaybackController(QObject* parent)
    : QObject(parent) {
    m_autoAdvanceTimer.setSingleShot(true);
    m_autoAdvanceTimer.setObjectName(QStringLiteral("bubblePlaybackTimer"));
    connect(&m_autoAdvanceTimer, &QTimer::timeout,
            this, &BubblePlaybackController::advanceAutomatically);
}

void BubblePlaybackController::reset(const QString& messageId) {
    const bool wasPaused = playbackPaused();
    m_autoAdvanceTimer.stop();
    m_messageId = messageId;
    m_sealedPages.clear();
    m_draftPage.clear();
    m_currentIndex = -1;
    m_hovered = false;
    m_userPaused = false;
    m_remainingAutoAdvanceMs = -1;
    if (wasPaused) emit playbackStateChanged(false);
}

void BubblePlaybackController::appendSealedPages(const QStringList& pages) {
    QStringList nonEmptyPages;
    for (const QString& page : pages) {
        if (!page.isEmpty()) nonEmptyPages.append(page);
    }
    if (nonEmptyPages.isEmpty()) return;

    const int oldSealedCount = m_sealedPages.size();
    const bool hadDraft = !m_draftPage.isEmpty();
    const bool currentWasDraft = hadDraft
        && m_currentIndex == oldSealedCount;
    m_draftPage.clear();
    m_sealedPages.append(nonEmptyPages);
    if (m_currentIndex < 0) {
        m_currentIndex = 0;
    } else if (currentWasDraft) {
        m_currentIndex = oldSealedCount;
    }
    emitCurrentPage();
    updatePlaybackState();
}

void BubblePlaybackController::updateDraftPage(const QString& page) {
    const bool viewerWasOnDraft = m_currentIndex < 0
        || currentPageIsDraft();
    const bool draftChanged = m_draftPage != page;
    if (!draftChanged) return;

    m_draftPage = page;
    const int newTotal = pageCount();
    if (newTotal == 0) {
        m_currentIndex = -1;
        stopAutoAdvance(false);
        return;
    }
    if (viewerWasOnDraft) m_currentIndex = newTotal - 1;
    m_currentIndex = std::clamp(m_currentIndex, 0, newTotal - 1);
    emitCurrentPage();
    updatePlaybackState();
}

void BubblePlaybackController::finishDraft() {
    if (m_draftPage.isEmpty()) return;
    const int draftIndex = m_sealedPages.size();
    m_sealedPages.append(m_draftPage);
    m_draftPage.clear();
    if (m_currentIndex < 0) m_currentIndex = draftIndex;
    emitCurrentPage();
    updatePlaybackState();
}

void BubblePlaybackController::setHovered(bool hovered) {
    if (m_hovered == hovered) return;
    const bool wasPaused = playbackPaused();
    m_hovered = hovered;
    if (hovered) {
        stopAutoAdvance(true);
    } else {
        updatePlaybackState();
    }
    if (wasPaused != playbackPaused()) {
        emit playbackStateChanged(playbackPaused());
    }
}

void BubblePlaybackController::toggleUserPause() {
    const bool wasPaused = playbackPaused();
    m_userPaused = !m_userPaused;
    if (m_userPaused) {
        stopAutoAdvance(false);
    } else {
        m_remainingAutoAdvanceMs = -1;
        updatePlaybackState(true);
    }
    if (wasPaused != playbackPaused()) {
        emit playbackStateChanged(playbackPaused());
    }
}

void BubblePlaybackController::previous() {
    if (m_currentIndex <= 0 || pageCount() <= 0) return;
    const bool wasPaused = playbackPaused();
    --m_currentIndex;
    m_userPaused = true;
    stopAutoAdvance(false);
    emitCurrentPage();
    if (wasPaused != playbackPaused()) emit playbackStateChanged(true);
}

void BubblePlaybackController::next() {
    if (m_currentIndex < 0 || m_currentIndex + 1 >= pageCount()) return;
    const bool wasPaused = playbackPaused();
    ++m_currentIndex;
    m_userPaused = true;
    stopAutoAdvance(false);
    emitCurrentPage();
    if (wasPaused != playbackPaused()) emit playbackStateChanged(true);
}

QString BubblePlaybackController::currentPageText() const {
    if (m_currentIndex < 0) return {};
    if (m_currentIndex < m_sealedPages.size()) {
        return m_sealedPages.at(m_currentIndex);
    }
    return currentPageIsDraft() ? m_draftPage : QString();
}

int BubblePlaybackController::pageCount() const {
    return m_sealedPages.size() + (m_draftPage.isEmpty() ? 0 : 1);
}

bool BubblePlaybackController::currentPageIsDraft() const {
    return !m_draftPage.isEmpty()
        && m_currentIndex == m_sealedPages.size();
}

bool BubblePlaybackController::hasUnreadPages() const {
    return m_currentIndex >= 0 && m_currentIndex + 1 < pageCount();
}

int BubblePlaybackController::remainingAutoAdvanceMs() const {
    return m_autoAdvanceTimer.isActive()
        ? m_autoAdvanceTimer.remainingTime()
        : m_remainingAutoAdvanceMs;
}

void BubblePlaybackController::emitCurrentPage() {
    if (m_currentIndex < 0 || m_currentIndex >= pageCount()) return;
    emit pageChanged(currentPageText(), m_currentIndex, pageCount(),
                     currentPageIsDraft());
}

void BubblePlaybackController::updatePlaybackState(bool resetDuration) {
    if (playbackPaused() || !hasUnreadPages()) {
        stopAutoAdvance(!resetDuration);
        return;
    }
    if (m_autoAdvanceTimer.isActive()) return;
    int duration = m_remainingAutoAdvanceMs;
    if (resetDuration || duration <= 0) {
        duration = readingDurationMs(currentPageText());
    }
    m_remainingAutoAdvanceMs = -1;
    m_autoAdvanceTimer.start(duration);
}

void BubblePlaybackController::stopAutoAdvance(bool preserveRemaining) {
    if (m_autoAdvanceTimer.isActive() && preserveRemaining) {
        m_remainingAutoAdvanceMs = std::max(
            1, m_autoAdvanceTimer.remainingTime());
    } else if (!preserveRemaining) {
        m_remainingAutoAdvanceMs = -1;
    }
    m_autoAdvanceTimer.stop();
}

void BubblePlaybackController::advanceAutomatically() {
    m_remainingAutoAdvanceMs = -1;
    if (playbackPaused() || !hasUnreadPages()) return;
    ++m_currentIndex;
    emitCurrentPage();
    updatePlaybackState(true);
}

int BubblePlaybackController::readingDurationMs(const QString& text) const {
    const int scalarCount = text.toUcs4().size();
    return std::clamp(1800 + 55 * scalarCount, 2400, 8500);
}
