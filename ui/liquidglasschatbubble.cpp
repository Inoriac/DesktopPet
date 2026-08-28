#include "liquidglasschatbubble.h"

#include "liquidglassmaterial.h"

#include <QFontMetrics>
#include <QEnterEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QMoveEvent>
#include <QShowEvent>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVariantAnimation>
#include <algorithm>

#include "ai_types.h"

namespace {
constexpr int kMaxBubbleWidth = 380;
constexpr int kMaxBubbleHeight = 400;
constexpr int kMinBubbleHeight = 48;
constexpr int kRefreshThrottleMs = 96;
constexpr int kControlsHeight = 40;
constexpr int kControlButtonSize = 32;
constexpr int kControlGap = 4;
} // namespace

LiquidGlassChatBubble::LiquidGlassChatBubble(QWidget* parent)
    : QWidget(parent) {
    setWindowFlag(Qt::FramelessWindowHint, true);
    setWindowFlag(Qt::Tool, true);
    setWindowFlag(Qt::WindowStaysOnTopHint, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setFocusPolicy(Qt::StrongFocus);
    setWindowOpacity(1.0);
    hide();

    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setInterval(kRefreshThrottleMs);
    connect(&m_refreshTimer, &QTimer::timeout, this, [this]() {
        m_refreshPending = false;
        analyzeAndApplyBackground();
    });

    m_input = new QLineEdit(this);
    m_input->hide();
    m_input->setFrame(false);
    m_input->installEventFilter(this);
    connect(m_input, &QLineEdit::textChanged, this, [this]() {
        if (!m_inputAutoFadeEnabled) return;
        setInputRevealed(inputShouldStayRevealed());
    });
    connect(m_input, &QLineEdit::returnPressed, this, [this]() {
        const QString value = m_input ? m_input->text().trimmed() : QString();
        if (m_inputSubmissionEnabled && !value.isEmpty()) {
            emit messageSubmitted(value);
            m_input->clear();
            m_input->clearFocus();
            if (m_inputAutoFadeEnabled && !inputShouldStayRevealed()) {
                setInputRevealed(false);
            }
        }
    });

    const auto createControl = [this](const QString& objectName,
                                      QStyle::StandardPixmap icon,
                                      const QString& tooltip) {
        auto* button = new QToolButton(this);
        button->setObjectName(objectName);
        button->setFixedSize(kControlButtonSize, kControlButtonSize);
        button->setIcon(style()->standardIcon(icon));
        button->setIconSize(QSize(17, 17));
        button->setAutoRaise(true);
        button->setToolTip(tooltip);
        button->setAccessibleName(tooltip);
        button->setFocusPolicy(Qt::NoFocus);
        button->hide();
        return button;
    };
    m_previousButton = createControl(
        QStringLiteral("previousPageButton"), QStyle::SP_ArrowLeft,
        QStringLiteral("上一页"));
    m_nextButton = createControl(
        QStringLiteral("nextPageButton"), QStyle::SP_ArrowRight,
        QStringLiteral("下一页"));
    m_playbackButton = createControl(
        QStringLiteral("playbackToggleButton"), QStyle::SP_MediaPause,
        QStringLiteral("暂停自动播放"));
    m_openButton = createControl(
        QStringLiteral("openConversationButton"), QStyle::SP_DirOpenIcon,
        QStringLiteral("打开完整对话"));
    connect(m_previousButton, &QToolButton::clicked,
            this, &LiquidGlassChatBubble::previousPageRequested);
    connect(m_nextButton, &QToolButton::clicked,
            this, &LiquidGlassChatBubble::nextPageRequested);
    connect(m_playbackButton, &QToolButton::clicked,
            this, &LiquidGlassChatBubble::playbackToggleRequested);
    connect(m_openButton, &QToolButton::clicked, this, [this]() {
        emit openConversationRequested(m_messageId);
    });
}

void LiquidGlassChatBubble::setMessage(const QString& message) {
    m_text = message.trimmed().isEmpty() ? QStringLiteral("...") : message.trimmed();
    m_inputMode = false;
    m_streamingMode = false;
    m_messageId.clear();
    m_activityText.clear();
    if (m_input) {
        m_input->hide();
    }
    updateOutputControlsState();
    resize(sizeHint());
    scheduleDynamicRefresh(true);
    update();
}

void LiquidGlassChatBubble::setLayoutReserveText(const QString& text) {
    m_layoutReserveText = text.trimmed();
    resize(sizeHint());
    updateInputGeometry();
    update();
}

void LiquidGlassChatBubble::setHasMorePages(bool hasMore) {
    if (m_hasMorePages == hasMore) {
        return;
    }
    m_hasMorePages = hasMore;
    update();
}

void LiquidGlassChatBubble::setInputAutoFadeEnabled(bool enabled) {
    m_inputAutoFadeEnabled = enabled;
    setMouseTracking(enabled);
    if (m_input) {
        m_input->setMouseTracking(enabled);
    }
    if (enabled) {
        setInputRevealed(false);
    } else {
        setInputRevealed(true);
    }
}

void LiquidGlassChatBubble::setInputSubmissionEnabled(bool enabled) {
    m_inputSubmissionEnabled = enabled;
}

void LiquidGlassChatBubble::showMessage(const QString& message) {
    setMessage(message);
    show();
    raise();
    scheduleDynamicRefresh(true);
}

void LiquidGlassChatBubble::showInput(const QString& placeholder, bool focusInput) {
    m_text.clear();
    m_layoutReserveText.clear();
    m_inputMode = true;
    m_streamingMode = false;
    m_messageId.clear();
    m_hasMorePages = false;
    resize(QSize(kMaxBubbleWidth, kMinBubbleHeight));
    updateInputGeometry();
    if (m_input) {
        m_input->setPlaceholderText(placeholder.isEmpty() ? QStringLiteral("和我说点什么...") : placeholder);
        m_input->show();
        if (focusInput) {
            m_input->setFocus(Qt::PopupFocusReason);
        }
    }
    if (m_inputAutoFadeEnabled) {
        setInputRevealed(focusInput || inputShouldStayRevealed());
    }
    updateOutputControlsState();
    show();
    raise();
    scheduleDynamicRefresh(true);
    update();
}

void LiquidGlassChatBubble::showStreamingMessage(const QString& messageId) {
    m_messageId = messageId;
    m_text.clear();
    m_layoutReserveText.clear();
    m_activityText.clear();
    m_inputMode = false;
    m_streamingMode = true;
    m_hasMorePages = false;
    m_displayedPageIndex = 0;
    m_displayedPageTotal = 0;
    m_displayedPageDraft = false;
    m_playbackPaused = false;
    if (m_input) m_input->hide();
    updateOutputControlsState();
    resize(sizeHint());
    show();
    raise();
    scheduleDynamicRefresh(true);
    update();
}

void LiquidGlassChatBubble::setDisplayedPage(const QString& text,
                                             int index,
                                             int total,
                                             bool draft) {
    m_text = text;
    m_displayedPageTotal = std::max(0, total);
    m_displayedPageIndex = m_displayedPageTotal > 0
        ? std::clamp(index, 0, m_displayedPageTotal - 1)
        : 0;
    m_displayedPageDraft = draft;
    updateOutputControlsState();
    resize(sizeHint());
    scheduleDynamicRefresh(false);
    update();
}

void LiquidGlassChatBubble::setActivityText(const QString& text) {
    if (m_activityText == text) return;
    m_activityText = text;
    resize(sizeHint());
    updateOutputControlsGeometry();
    update();
}

void LiquidGlassChatBubble::setPlaybackPaused(bool paused) {
    if (m_playbackPaused == paused) return;
    m_playbackPaused = paused;
    updateOutputControlsState();
}

void LiquidGlassChatBubble::hideBubble() {
    hide();
    if (m_input) {
        m_input->hide();
    }
    emit dismissed();
}

void LiquidGlassChatBubble::applyScreenChatConfig(const ScreenChatConfig& config) {
    m_opacityPercent = std::clamp(config.bubbleOpacityPercent, 10, 100);
    m_fontSize = std::clamp(config.bubbleFontSize, 10, 36);
    if (m_input) {
        QFont f = m_input->font();
        f.setPointSize(m_fontSize);
        m_input->setFont(f);
    }
    resize(sizeHint());
    updateInputGeometry();
    scheduleDynamicRefresh(true);
    update();
}

void LiquidGlassChatBubble::refreshGlass() {
    scheduleDynamicRefresh(true);
}

void LiquidGlassChatBubble::scheduleDynamicRefresh(bool immediate) {
    if (size().isEmpty() || (!immediate && !isVisible())) {
        return;
    }

    if (immediate) {
        m_refreshPending = false;
        m_refreshTimer.stop();
        analyzeAndApplyBackground();
        return;
    }

    if (!m_refreshTimer.isActive()) {
        m_refreshTimer.start();
    } else {
        m_refreshPending = true;
    }
}

QSize LiquidGlassChatBubble::sizeHint() const {
    if (m_inputMode) {
        return QSize(kMaxBubbleWidth, kMinBubbleHeight);
    }

    QFont f = font();
    f.setPointSize(m_fontSize);
    f.setWeight(QFont::DemiBold);
    const QFontMetrics fm(f);
    QString layoutText = m_layoutReserveText.isEmpty() ? m_text : m_layoutReserveText;
    if (layoutText.isEmpty()) layoutText = m_activityText;
    const QRect textRect = fm.boundingRect(QRect(0, 0, contentWidth(), 1000),
                                           Qt::TextWordWrap | Qt::AlignCenter,
                                           layoutText.isEmpty() ? QStringLiteral("...") : layoutText);
    const int indicatorPadding = m_hasMorePages ? 16 : 0;
    const int controlsPadding = m_streamingMode ? kControlsHeight : 0;
    return QSize(kMaxBubbleWidth,
                 std::clamp(textRect.height() + m_paddingV * 2
                                + indicatorPadding + controlsPadding,
                            kMinBubbleHeight, kMaxBubbleHeight));
}

void LiquidGlassChatBubble::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

    const QRect localRect = rect().adjusted(1, 1, -1, -1);
    QImage background = m_backgroundCache;
    if (background.isNull()) {
        background = QImage(size(), QImage::Format_ARGB32_Premultiplied);
        background.fill(QColor(245, 248, 252, 190));
    }

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setCompositionMode(QPainter::CompositionMode_Clear);
    p.fillRect(rect(), Qt::transparent);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);

    QPainterPath path;
    path.addRoundedRect(localRect, m_radius, m_radius);
    p.setClipPath(path);
    p.drawImage(rect(), background);
    p.fillRect(rect(), m_materialColor);

    QLinearGradient shine(0, 0, 0, height());
    shine.setColorAt(0.0, QColor(255, 255, 255, 30));
    shine.setColorAt(0.45, QColor(255, 255, 255, 10));
    shine.setColorAt(1.0, QColor(255, 255, 255, 4));
    p.fillRect(rect(), shine);

    p.setClipping(false);
    p.setPen(QColor(255, 255, 255, 90));
    p.drawRoundedRect(localRect, m_radius, m_radius);

    if (!m_inputMode) {
        QRect textRect = displayedTextRect();
        if (m_hasMorePages) {
            textRect.adjust(0, 0, 0, -16);
        }
        const QString displayedText = m_text.isEmpty()
            ? m_activityText : m_text;
        p.setFont(fittedOutputFont(textRect, displayedText));
        constexpr int textFlags = Qt::TextWordWrap | Qt::TextWrapAnywhere
            | Qt::AlignCenter;
        const QColor shadow = m_textColor.lightness() > 127 ? QColor(0, 0, 0, 130) : QColor(255, 255, 255, 120);
        p.setPen(shadow);
        p.drawText(textRect.translated(0, m_shadowOffset), textFlags,
                   displayedText);
        p.setPen(m_textColor);
        p.drawText(textRect, textFlags, displayedText);

        if (m_hasMorePages) {
            const QRect indicator = moreIndicatorRect();
            QPainterPath triangle;
            triangle.moveTo(indicator.left(), indicator.top());
            triangle.lineTo(indicator.right(), indicator.top());
            triangle.lineTo(indicator.center().x(), indicator.bottom());
            triangle.closeSubpath();

            p.setRenderHint(QPainter::Antialiasing, true);
            p.setBrush(m_textColor);
            p.setPen(Qt::NoPen);
            p.drawPath(triangle);
        }

        if (m_streamingMode) {
            QFont captionFont = font();
            captionFont.setPointSize(std::max(9, m_fontSize - 3));
            captionFont.setWeight(QFont::Normal);
            p.setFont(captionFont);
            p.setPen(QColor(m_textColor.red(), m_textColor.green(),
                            m_textColor.blue(), 175));
            const QRect controls = playbackControlsRect();
            const int buttonsWidth = kControlButtonSize * 4
                + kControlGap * 3;
            const QRect captionRect(
                controls.left(), controls.top(),
                std::max(0, controls.width() - buttonsWidth - 8),
                controls.height());
            QString caption = m_text.isEmpty() ? QString() : m_activityText;
            if (m_displayedPageTotal > 0) {
                const QString counter = QStringLiteral("%1/%2")
                    .arg(m_displayedPageIndex + 1)
                    .arg(m_displayedPageTotal);
                caption = caption.isEmpty()
                    ? counter : QStringLiteral("%1  %2").arg(caption, counter);
            }
            p.drawText(captionRect, Qt::AlignVCenter | Qt::AlignLeft,
                       QFontMetrics(captionFont).elidedText(
                           caption, Qt::ElideRight, captionRect.width()));
        }
    }
}

void LiquidGlassChatBubble::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    scheduleDynamicRefresh(true);
}

void LiquidGlassChatBubble::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateInputGeometry();
    updateOutputControlsGeometry();
    scheduleDynamicRefresh(true);
}

void LiquidGlassChatBubble::moveEvent(QMoveEvent* event) {
    QWidget::moveEvent(event);
    scheduleDynamicRefresh(false);
}

void LiquidGlassChatBubble::mousePressEvent(QMouseEvent* event) {
    if (m_inputMode && m_input) {
        setInputRevealed(true);
        m_input->setFocus(Qt::MouseFocusReason);
    }
    if (!m_inputMode && m_hasMorePages && moreIndicatorRect().adjusted(-8, -8, 8, 8).contains(event->pos())) {
        emit morePagesRequested();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void LiquidGlassChatBubble::enterEvent(QEnterEvent* event) {
    if (m_inputMode && m_inputAutoFadeEnabled) {
        setInputRevealed(true);
    }
    if (m_streamingMode) emit hoveredChanged(true);
    QWidget::enterEvent(event);
}

void LiquidGlassChatBubble::leaveEvent(QEvent* event) {
    if (m_inputMode && m_inputAutoFadeEnabled) {
        setInputRevealed(inputShouldStayRevealed());
    }
    if (m_streamingMode) emit hoveredChanged(false);
    QWidget::leaveEvent(event);
}

bool LiquidGlassChatBubble::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_input && m_inputAutoFadeEnabled) {
        if (event->type() == QEvent::FocusIn) {
            setInputRevealed(true);
        } else if (event->type() == QEvent::FocusOut) {
            QTimer::singleShot(0, this, [this]() {
                setInputRevealed(inputShouldStayRevealed());
            });
        }
    }
    return QWidget::eventFilter(watched, event);
}

void LiquidGlassChatBubble::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        hideBubble();
        return;
    }
    QWidget::keyPressEvent(event);
}

void LiquidGlassChatBubble::analyzeAndApplyBackground() {
    if (size().isEmpty()) {
        return;
    }

    const LiquidGlassMaterialSample sample = LiquidGlassMaterialAnalyzer::analyze(this, m_opacityPercent, m_textColor);
    if (!sample.valid) {
        return;
    }

    m_backgroundCache = sample.background;

    if (LiquidGlassMaterialAnalyzer::colorDistance(sample.materialColor, m_materialColor) < 4.0
        && sample.textColor == m_textColor) {
        update();
        if (m_refreshPending) {
            m_refreshPending = false;
            m_refreshTimer.start();
        }
        return;
    }

    animateMaterialTo(sample.materialColor, sample.textColor);
    if (m_refreshPending) {
        m_refreshPending = false;
        m_refreshTimer.start();
    }
}

void LiquidGlassChatBubble::animateMaterialTo(const QColor& materialColor, const QColor& textColor) {
    if (!m_materialAnimation) {
        m_materialAnimation = new QVariantAnimation(this);
        m_materialAnimation->setDuration(180);
        connect(m_materialAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
            const qreal t = value.toReal();
            m_materialColor = LiquidGlassMaterialAnalyzer::blendColors(m_animationStartMaterial, m_animationEndMaterial, t);
            m_textColor = LiquidGlassMaterialAnalyzer::blendColors(m_animationStartText, m_animationEndText, t);
            updateTextPalette(m_textColor);
            update();
        });
        connect(m_materialAnimation, &QVariantAnimation::finished, this, [this]() {
            m_materialColor = m_animationEndMaterial;
            m_textColor = m_animationEndText;
            updateTextPalette(m_textColor);
            update();
        });
    }

    m_materialAnimation->stop();
    m_animationStartMaterial = m_materialColor;
    m_animationEndMaterial = materialColor;
    m_animationStartText = m_textColor;
    m_animationEndText = textColor;
    m_materialAnimation->setStartValue(0.0);
    m_materialAnimation->setEndValue(1.0);
    m_materialAnimation->start();
}
