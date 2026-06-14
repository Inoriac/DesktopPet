#ifndef DESKTOP_PET_LIQUIDGLASSCHATBUBBLE_H
#define DESKTOP_PET_LIQUIDGLASSCHATBUBBLE_H

#include <QColor>
#include <QImage>
#include <QLineEdit>
#include <QPropertyAnimation>
#include <QPointer>
#include <QTimer>
#include <QVariantAnimation>
#include <QWidget>

struct ScreenChatConfig;

class QEnterEvent;
class QEvent;
class QMouseEvent;
class QMoveEvent;
class QShowEvent;

class LiquidGlassChatBubble : public QWidget {
    Q_OBJECT

public:
    explicit LiquidGlassChatBubble(QWidget* parent = nullptr);

    void setMessage(const QString& message);
    void setLayoutReserveText(const QString& text);
    void setHasMorePages(bool hasMore);
    void setInputAutoFadeEnabled(bool enabled);
    void showMessage(const QString& message);
    void showInput(const QString& placeholder = QString(), bool focusInput = true);
    void hideBubble();
    void applyScreenChatConfig(const ScreenChatConfig& config);
    void refreshGlass();
    void scheduleDynamicRefresh(bool immediate = false);

    QString text() const { return m_text; }
    QSize sizeHint() const override;

signals:
    void messageSubmitted(const QString& text);
    void morePagesRequested();
    void dismissed();

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void analyzeAndApplyBackground();
    void animateMaterialTo(const QColor& materialColor, const QColor& textColor);
    QRect moreIndicatorRect() const;
    void setInputRevealed(bool revealed);
    void updateTextPalette(const QColor& color);
    void updateInputGeometry();
    int contentWidth() const;

private:
    QString m_text;
    QString m_layoutReserveText;
    bool m_inputMode = false;
    bool m_hasMorePages = false;
    bool m_inputAutoFadeEnabled = false;
    bool m_inputRevealed = true;
    bool m_refreshPending = false;
    QPointer<QLineEdit> m_input;
    QPointer<QPropertyAnimation> m_opacityAnimation;
    QTimer m_refreshTimer;
    QPointer<QVariantAnimation> m_materialAnimation;
    QImage m_backgroundCache;
    QColor m_materialColor = QColor(245, 248, 252, 178);
    QColor m_textColor = Qt::black;
    QColor m_animationStartMaterial;
    QColor m_animationEndMaterial;
    QColor m_animationStartText;
    QColor m_animationEndText;

    int m_fontSize = 14;
    int m_opacityPercent = 80;
    int m_radius = 18;
    int m_paddingH = 16;
    int m_paddingV = 11;
    int m_shadowOffset = 1;
};

#endif // DESKTOP_PET_LIQUIDGLASSCHATBUBBLE_H
