#ifndef DESKTOP_PET_ANIMATED_SCROLLBAR_H
#define DESKTOP_PET_ANIMATED_SCROLLBAR_H

#include <QScrollBar>

class QPropertyAnimation;
class QTimer;

class AnimatedScrollBar : public QScrollBar {
    Q_OBJECT
    Q_PROPERTY(qreal revealProgress READ revealProgress WRITE setRevealProgress)

public:
    explicit AnimatedScrollBar(Qt::Orientation orientation, QWidget* parent = nullptr);

    qreal revealProgress() const;
    void setRevealProgress(qreal progress);

protected:
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void setExpanded(bool expanded);
    void revealTemporarily();
    QRectF handleRect(qreal handleThickness) const;

    QPropertyAnimation* m_revealAnimation = nullptr;
    QTimer* m_collapseTimer = nullptr;
    qreal m_revealProgress = 0.0;
    bool m_hovered = false;
    bool m_dragging = false;
};

#endif // DESKTOP_PET_ANIMATED_SCROLLBAR_H
