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
#include <QTimer>
#include <QVariantAnimation>
#include <algorithm>

#include "ai_types.h"

namespace {
constexpr int kMaxBubbleWidth = 380;
constexpr int kMaxBubbleHeight = 400;
constexpr int kMinBubbleHeight = 48;
constexpr int kRefreshThrottleMs = 96;
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
    connect(m_input, &QLineEdit::returnPressed, this, [this]() {
        const QString value = m_input ? m_input->text().trimmed() : QString();
        if (!value.isEmpty()) {
            emit messageSubmitted(value);
            m_input->clear();
            m_input->clearFocus();
            if (m_inputAutoFadeEnabled && !underMouse()) {
                setInputRevealed(false);
            }
        }
    });
}

void LiquidGlassChatBubble::setMessage(const QString& message) {
    m_text = message.trimmed().isEmpty() ? QStringLiteral("...") : message.trimmed();
    m_inputMode = false;
    if (m_input) {
        m_input->hide();
    }
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
        setInputRevealed(focusInput || underMouse());
    }
    show();
    raise();
    scheduleDynamicRefresh(true);
    update();
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
    const QString layoutText = m_layoutReserveText.isEmpty() ? m_text : m_layoutReserveText;
    const QRect textRect = fm.boundingRect(QRect(0, 0, contentWidth(), 1000),
                                           Qt::TextWordWrap | Qt::AlignCenter,
                                           layoutText.isEmpty() ? QStringLiteral("...") : layoutText);
    const int indicatorPadding = m_hasMorePages ? 16 : 0;
    return QSize(kMaxBubbleWidth,
                 std::clamp(textRect.height() + m_paddingV * 2 + indicatorPadding, kMinBubbleHeight, kMaxBubbleHeight));
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
        QFont f = font();
        f.setPointSize(m_fontSize);
        f.setWeight(QFont::DemiBold);
        p.setFont(f);

        QRect textRect = localRect.adjusted(m_paddingH, m_paddingV, -m_paddingH, -m_paddingV);
        if (m_hasMorePages) {
            textRect.adjust(0, 0, 0, -16);
        }
        const QColor shadow = m_textColor.lightness() > 127 ? QColor(0, 0, 0, 130) : QColor(255, 255, 255, 120);
        p.setPen(shadow);
        p.drawText(textRect.translated(0, m_shadowOffset), Qt::TextWordWrap | Qt::AlignCenter, m_text);
        p.setPen(m_textColor);
        p.drawText(textRect, Qt::TextWordWrap | Qt::AlignCenter, m_text);

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
    }
}

void LiquidGlassChatBubble::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    scheduleDynamicRefresh(true);
}

void LiquidGlassChatBubble::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateInputGeometry();
    scheduleDynamicRefresh(true);
}

void LiquidGlassChatBubble::moveEvent(QMoveEvent* event) {
    QWidget::moveEvent(event);
    scheduleDynamicRefresh(false);
}

void LiquidGlassChatBubble::mousePressEvent(QMouseEvent* event) {
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
    QWidget::enterEvent(event);
}

void LiquidGlassChatBubble::leaveEvent(QEvent* event) {
    if (m_inputMode && m_inputAutoFadeEnabled) {
        setInputRevealed(false);
    }
    QWidget::leaveEvent(event);
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

