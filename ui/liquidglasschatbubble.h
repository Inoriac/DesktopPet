#ifndef DESKTOP_PET_LIQUIDGLASSCHATBUBBLE_H
#define DESKTOP_PET_LIQUIDGLASSCHATBUBBLE_H

#include <QColor>
#include <QImage>
#include <QLineEdit>
#include <QPointer>
#include <QWidget>

struct ScreenChatConfig;

class LiquidGlassChatBubble : public QWidget {
    Q_OBJECT

public:
    explicit LiquidGlassChatBubble(QWidget* parent = nullptr);

    void setMessage(const QString& message);
    void showMessage(const QString& message);
    void showInput(const QString& placeholder = QString(), bool focusInput = true);
    void hideBubble();
    void applyScreenChatConfig(const ScreenChatConfig& config);
    void refreshGlass();

    QString text() const { return m_text; }
    QSize sizeHint() const override;

signals:
    void messageSubmitted(const QString& text);
    void dismissed();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    QImage captureBackground(const QRect& globalRect) const;
    QImage makeBlurredGlass(const QImage& source) const;
    QColor chooseReadableTextColor(const QImage& glassImage) const;
    void updateTextPalette(const QColor& color);
    void updateInputGeometry();
    int contentWidth() const;

private:
    QString m_text;
    bool m_inputMode = false;
    QPointer<QLineEdit> m_input;
    QImage m_glassCache;
    QColor m_textColor = Qt::black;

    int m_fontSize = 14;
    int m_opacityPercent = 80;
    int m_radius = 18;
    int m_paddingH = 16;
    int m_paddingV = 11;
    int m_shadowOffset = 1;
};

#endif // DESKTOP_PET_LIQUIDGLASSCHATBUBBLE_H
