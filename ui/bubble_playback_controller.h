#ifndef DESKTOP_PET_BUBBLE_PLAYBACK_CONTROLLER_H
#define DESKTOP_PET_BUBBLE_PLAYBACK_CONTROLLER_H

#include <QObject>
#include <QStringList>
#include <QTimer>

class BubblePlaybackController : public QObject {
    Q_OBJECT

public:
    explicit BubblePlaybackController(QObject* parent = nullptr);

    void reset(const QString& messageId);
    void appendSealedPages(const QStringList& pages);
    void updateDraftPage(const QString& page);
    void finishDraft();
    void setHovered(bool hovered);
    void toggleUserPause();
    void previous();
    void next();

    QString messageId() const { return m_messageId; }
    QString currentPageText() const;
    int currentIndex() const { return m_currentIndex; }
    int pageCount() const;
    bool currentPageIsDraft() const;
    bool isUserPaused() const { return m_userPaused; }
    bool isHovered() const { return m_hovered; }
    bool hasUnreadPages() const;
    bool autoAdvanceActive() const { return m_autoAdvanceTimer.isActive(); }
    int remainingAutoAdvanceMs() const;

signals:
    void pageChanged(const QString& text, int index, int total, bool draft);
    void playbackStateChanged(bool paused);

private:
    void emitCurrentPage();
    void updatePlaybackState(bool resetDuration = false);
    void stopAutoAdvance(bool preserveRemaining);
    void advanceAutomatically();
    int readingDurationMs(const QString& text) const;
    bool playbackPaused() const { return m_userPaused || m_hovered; }

    QString m_messageId;
    QStringList m_sealedPages;
    QString m_draftPage;
    int m_currentIndex = -1;
    bool m_hovered = false;
    bool m_userPaused = false;
    int m_remainingAutoAdvanceMs = -1;
    QTimer m_autoAdvanceTimer;
};

#endif // DESKTOP_PET_BUBBLE_PLAYBACK_CONTROLLER_H
