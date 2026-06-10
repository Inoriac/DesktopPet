#ifndef DESKTOP_PET_CARD_WIDGET_H
#define DESKTOP_PET_CARD_WIDGET_H

#include <QWidget>

class QLabel;
class QFrame;
class QVBoxLayout;
class QWidget;

class CardWidget : public QWidget {
    Q_OBJECT

public:
    explicit CardWidget(const QString& title,
                        const QString& subtitle = QString(),
                        QWidget* parent = nullptr);

    QVBoxLayout* contentLayout() const;
    void addWidget(QWidget* widget);

private:
    QWidget* m_headerWidget = nullptr;
    QWidget* m_bodyWidget = nullptr;
    QLabel* m_titleLabel = nullptr;
    QLabel* m_subtitleLabel = nullptr;
    QVBoxLayout* m_contentLayout = nullptr;
    bool m_hasContent = false;
};

#endif // DESKTOP_PET_CARD_WIDGET_H
