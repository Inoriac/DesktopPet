#include "animated_scrollbar.h"

#include "theme_manager.h"

#include <QEasingCurve>
#include <QMouseEvent>
#include <QPainter>
#include <QPropertyAnimation>
#include <QTimer>

#include <algorithm>

AnimatedScrollBar::AnimatedScrollBar(Qt::Orientation orientation, QWidget* parent)
    : QScrollBar(orientation, parent) {
    setObjectName(QStringLiteral("AnimatedScrollBar"));
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setMouseTracking(true);
    setFixedWidth(12);

    m_revealAnimation = new QPropertyAnimation(this, "revealProgress", this);
    m_revealAnimation->setDuration(180);
    m_revealAnimation->setEasingCurve(QEasingCurve::OutCubic);

    m_collapseTimer = new QTimer(this);
    m_collapseTimer->setSingleShot(true);
    m_collapseTimer->setInterval(700);
    connect(m_collapseTimer, &QTimer::timeout, this, [this]() {
        if (!m_hovered && !m_dragging) {
            setExpanded(false);
        }
    });

    connect(this, &QScrollBar::valueChanged, this, [this]() {
        revealTemporarily();
    });
    connect(this, &QScrollBar::rangeChanged, this, [this]() {
        update();
    });
}

qreal AnimatedScrollBar::revealProgress() const {
    return m_revealProgress;
}

void AnimatedScrollBar::setRevealProgress(qreal progress) {
    m_revealProgress = std::clamp(progress, 0.0, 1.0);
    update();
}

void AnimatedScrollBar::enterEvent(QEnterEvent* event) {
    m_hovered = true;
    setExpanded(true);
    QScrollBar::enterEvent(event);
}

void AnimatedScrollBar::leaveEvent(QEvent* event) {
    m_hovered = false;
    if (!m_dragging && m_collapseTimer) {
        m_collapseTimer->start();
    }
    QScrollBar::leaveEvent(event);
}

void AnimatedScrollBar::mousePressEvent(QMouseEvent* event) {
    m_dragging = true;
    setExpanded(true);
    QScrollBar::mousePressEvent(event);
}

void AnimatedScrollBar::mouseReleaseEvent(QMouseEvent* event) {
    QScrollBar::mouseReleaseEvent(event);
    m_dragging = false;
    if (!m_hovered && m_collapseTimer) {
        m_collapseTimer->start();
    }
}

void AnimatedScrollBar::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)

    if (maximum() <= minimum()) {
        return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const bool dark = ThemeManager::instance().isDarkTheme();
    const qreal progress = m_revealProgress;
    const qreal trackThickness = 3.0 + progress * 5.0;
    const qreal handleThickness = 4.0 + progress * 4.0;

    const QRectF trackRect(width() - trackThickness - 2.0,
                           6.0,
                           trackThickness,
                           height() - 12.0);
    QColor trackColor = dark ? QColor(255, 255, 255, 8 + static_cast<int>(progress * 16))
                             : QColor(32, 33, 36, 6 + static_cast<int>(progress * 14));
    painter.setPen(Qt::NoPen);
    painter.setBrush(trackColor);
    painter.drawRoundedRect(trackRect, trackThickness / 2.0, trackThickness / 2.0);

    QRectF thumbRect = handleRect(handleThickness);
    QColor handleColor = dark ? QColor(203, 213, 225, 74 + static_cast<int>(progress * 76))
                              : QColor(52, 64, 84, 64 + static_cast<int>(progress * 82));
    painter.setBrush(handleColor);
    painter.drawRoundedRect(thumbRect, handleThickness / 2.0, handleThickness / 2.0);
}

void AnimatedScrollBar::setExpanded(bool expanded) {
    if (!m_revealAnimation) {
        setRevealProgress(expanded ? 1.0 : 0.0);
        return;
    }
    m_revealAnimation->stop();
    m_revealAnimation->setStartValue(m_revealProgress);
    m_revealAnimation->setEndValue(expanded ? 1.0 : 0.0);
    m_revealAnimation->start();
}

void AnimatedScrollBar::revealTemporarily() {
    setExpanded(true);
    if (!m_hovered && !m_dragging && m_collapseTimer) {
        m_collapseTimer->start();
    }
}

QRectF AnimatedScrollBar::handleRect(qreal handleThickness) const {
    const qreal availableHeight = std::max(1, height() - 12);
    const qreal total = static_cast<qreal>(maximum() - minimum() + pageStep());
    const qreal ratio = total > 0 ? std::clamp(static_cast<qreal>(pageStep()) / total, 0.08, 1.0) : 1.0;
    const qreal handleHeight = std::max(34.0, availableHeight * ratio);
    const qreal travel = std::max(0.0, availableHeight - handleHeight);
    const qreal valueRatio = maximum() > minimum()
        ? static_cast<qreal>(value() - minimum()) / static_cast<qreal>(maximum() - minimum())
        : 0.0;
    const qreal y = 6.0 + travel * std::clamp(valueRatio, 0.0, 1.0);
    return QRectF(width() - handleThickness - 2.0, y, handleThickness, handleHeight);
}
