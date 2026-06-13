#include "liquidglasschatbubble.h"

#include <QFontMetrics>
#include <QGuiApplication>
#include <QEnterEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QScreen>
#include <QAbstractAnimation>
#include <algorithm>
#include <cmath>

#include "ai_types.h"

namespace {
constexpr int kMaxBubbleWidth = 380;
constexpr int kMaxBubbleHeight = 400;
constexpr int kMinBubbleHeight = 48;
constexpr int kBlurRadius = 12;

qreal srgbToLinear(qreal channel) {
    channel /= 255.0;
    return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
}

qreal relativeLuminance(const QColor& color) {
    return 0.2126 * srgbToLinear(color.red())
         + 0.7152 * srgbToLinear(color.green())
         + 0.0722 * srgbToLinear(color.blue());
}

qreal contrastRatio(const QColor& a, const QColor& b) {
    const qreal l1 = relativeLuminance(a);
    const qreal l2 = relativeLuminance(b);
    const qreal lighter = std::max(l1, l2);
    const qreal darker = std::min(l1, l2);
    return (lighter + 0.05) / (darker + 0.05);
}

QImage boxBlur(const QImage& src, int radius) {
    if (src.isNull() || radius <= 0) {
        return src;
    }

    const QImage input = src.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QImage temp(input.size(), input.format());
    QImage out(input.size(), input.format());
    const int w = input.width();
    const int h = input.height();

    for (int y = 0; y < h; ++y) {
        const QRgb* inLine = reinterpret_cast<const QRgb*>(input.constScanLine(y));
        QRgb* tempLine = reinterpret_cast<QRgb*>(temp.scanLine(y));
        int a = 0, r = 0, g = 0, b = 0;
        for (int x = -radius; x <= radius; ++x) {
            const QRgb px = inLine[std::clamp(x, 0, w - 1)];
            a += qAlpha(px); r += qRed(px); g += qGreen(px); b += qBlue(px);
        }
        const int count = radius * 2 + 1;
        for (int x = 0; x < w; ++x) {
            tempLine[x] = qRgba(r / count, g / count, b / count, a / count);
            const QRgb remove = inLine[std::clamp(x - radius, 0, w - 1)];
            const QRgb add = inLine[std::clamp(x + radius + 1, 0, w - 1)];
            a += qAlpha(add) - qAlpha(remove);
            r += qRed(add) - qRed(remove);
            g += qGreen(add) - qGreen(remove);
            b += qBlue(add) - qBlue(remove);
        }
    }

    for (int x = 0; x < w; ++x) {
        int a = 0, r = 0, g = 0, b = 0;
        for (int y = -radius; y <= radius; ++y) {
            const QRgb px = reinterpret_cast<const QRgb*>(temp.constScanLine(std::clamp(y, 0, h - 1)))[x];
            a += qAlpha(px); r += qRed(px); g += qGreen(px); b += qBlue(px);
        }
        const int count = radius * 2 + 1;
        for (int y = 0; y < h; ++y) {
            reinterpret_cast<QRgb*>(out.scanLine(y))[x] = qRgba(r / count, g / count, b / count, a / count);
            const QRgb remove = reinterpret_cast<const QRgb*>(temp.constScanLine(std::clamp(y - radius, 0, h - 1)))[x];
            const QRgb add = reinterpret_cast<const QRgb*>(temp.constScanLine(std::clamp(y + radius + 1, 0, h - 1)))[x];
            a += qAlpha(add) - qAlpha(remove);
            r += qRed(add) - qRed(remove);
            g += qGreen(add) - qGreen(remove);
            b += qBlue(add) - qBlue(remove);
        }
    }

    return out;
}
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
    update();
}

void LiquidGlassChatBubble::refreshGlass() {
    if (size().isEmpty()) {
        return;
    }

    m_glassCache = makeBlurredGlass(QRect(mapToGlobal(QPoint(0, 0)), size()).isEmpty()
        ? QImage()
        : captureBackground(QRect(mapToGlobal(QPoint(0, 0)), size())));
    m_textColor = chooseReadableTextColor(m_glassCache);
    updateTextPalette(m_textColor);
    update();
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
    QImage glass = m_glassCache;
    if (glass.isNull()) {
        glass = QImage(size(), QImage::Format_ARGB32_Premultiplied);
        glass.fill(QColor(245, 248, 252, 210));
    }

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setCompositionMode(QPainter::CompositionMode_Clear);
    p.fillRect(rect(), Qt::transparent);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);

    QPainterPath path;
    path.addRoundedRect(localRect, m_radius, m_radius);
    p.setClipPath(path);
    p.drawImage(rect(), glass);

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
        const QColor shadow = m_textColor == Qt::white ? QColor(0, 0, 0, 130) : QColor(255, 255, 255, 120);
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

void LiquidGlassChatBubble::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateInputGeometry();
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

QImage LiquidGlassChatBubble::captureBackground(const QRect& globalRect) const {
    QScreen* screen = QGuiApplication::screenAt(globalRect.center());
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }
    if (!screen) {
        return QImage(size(), QImage::Format_ARGB32_Premultiplied);
    }

    const QPixmap pixmap = screen->grabWindow(0,
                                              globalRect.x(),
                                              globalRect.y(),
                                              globalRect.width(),
                                              globalRect.height());

    if (pixmap.isNull()) {
        return QImage(size(), QImage::Format_ARGB32_Premultiplied);
    }
    return pixmap.toImage();
}

QImage LiquidGlassChatBubble::makeBlurredGlass(const QImage& source) const {
    QImage blurred = boxBlur(source, kBlurRadius);
    QPainter painter(&blurred);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

    const QColor avg = chooseReadableTextColor(blurred) == Qt::white
        ? QColor(0, 0, 0, std::clamp(65 + (100 - m_opacityPercent), 45, 110))
        : QColor(255, 255, 255, std::clamp(55 + m_opacityPercent / 2, 55, 115));
    painter.fillRect(blurred.rect(), avg);

    QLinearGradient shine(0, 0, 0, blurred.height());
    shine.setColorAt(0.0, QColor(255, 255, 255, 42));
    shine.setColorAt(1.0, QColor(255, 255, 255, 8));
    painter.fillRect(blurred.rect(), shine);
    return blurred;
}

QColor LiquidGlassChatBubble::chooseReadableTextColor(const QImage& glassImage) const {
    if (glassImage.isNull()) {
        return Qt::black;
    }

    qint64 r = 0, g = 0, b = 0, count = 0;
    const int step = std::max(1, std::min(glassImage.width(), glassImage.height()) / 48);
    for (int y = 0; y < glassImage.height(); y += step) {
        for (int x = 0; x < glassImage.width(); x += step) {
            const QColor c = QColor::fromRgb(glassImage.pixel(x, y));
            r += c.red(); g += c.green(); b += c.blue(); ++count;
        }
    }

    const QColor avg(static_cast<int>(r / std::max<qint64>(1, count)),
                     static_cast<int>(g / std::max<qint64>(1, count)),
                     static_cast<int>(b / std::max<qint64>(1, count)));
    const qreal blackContrast = contrastRatio(Qt::black, avg);
    const qreal whiteContrast = contrastRatio(Qt::white, avg);
    return whiteContrast >= blackContrast ? Qt::white : Qt::black;
}

QRect LiquidGlassChatBubble::moreIndicatorRect() const {
    return QRect(width() - m_paddingH - 14, height() - m_paddingV - 10, 14, 10);
}

void LiquidGlassChatBubble::setInputRevealed(bool revealed) {
    if (m_inputRevealed == revealed && m_opacityAnimation && m_opacityAnimation->state() == QAbstractAnimation::Stopped) {
        return;
    }

    m_inputRevealed = revealed;
    const qreal targetOpacity = revealed ? 1.0 : 0.0;
    if (!isVisible()) {
        if (m_opacityAnimation) {
            m_opacityAnimation->stop();
        }
        setWindowOpacity(targetOpacity);
        return;
    }

    if (!m_opacityAnimation) {
        m_opacityAnimation = new QPropertyAnimation(this, "windowOpacity", this);
        m_opacityAnimation->setDuration(200);
    }
    m_opacityAnimation->stop();
    m_opacityAnimation->setStartValue(windowOpacity());
    m_opacityAnimation->setEndValue(targetOpacity);
    m_opacityAnimation->start();
}

void LiquidGlassChatBubble::updateTextPalette(const QColor& color) {
    if (!m_input) {
        return;
    }
    QPalette pal = m_input->palette();
    pal.setColor(QPalette::Text, color);
    pal.setColor(QPalette::PlaceholderText, QColor(color.red(), color.green(), color.blue(), 150));
    m_input->setPalette(pal);
    m_input->setStyleSheet(QStringLiteral("QLineEdit { background: transparent; border: none; }") );
}

void LiquidGlassChatBubble::updateInputGeometry() {
    if (!m_input) {
        return;
    }
    m_input->setGeometry(rect().adjusted(m_paddingH, 8, -m_paddingH, -8));
}

int LiquidGlassChatBubble::contentWidth() const {
    return kMaxBubbleWidth - m_paddingH * 2;
}
