#ifndef DESKTOP_PET_NAVIGATION_WIDGET_H
#define DESKTOP_PET_NAVIGATION_WIDGET_H

#include <QWidget>

#include <QHash>
#include <QString>

class QButtonGroup;
class QFrame;
class QPropertyAnimation;
class QPushButton;
class QVBoxLayout;

class NavigationWidget : public QWidget {
    Q_OBJECT
    Q_PROPERTY(int railWidth READ railWidth WRITE setRailWidth)

public:
    explicit NavigationWidget(QWidget* parent = nullptr);

    int railWidth() const;
    void setRailWidth(int width);
    void addItem(const QString& id, const QString& text);
    void setCurrentItem(const QString& id);

signals:
    void navigationRequested(const QString& id);

protected:
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    QPushButton* createButton(const QString& id, const QString& text);
    void animateRailWidth(int targetWidth);
    void moveSelectionTo(const QString& id, bool animated);

    QVBoxLayout* m_layout = nullptr;
    QButtonGroup* m_buttonGroup = nullptr;
    QPropertyAnimation* m_widthAnimation = nullptr;
    QPropertyAnimation* m_selectionAnimation = nullptr;
    QFrame* m_selectionPill = nullptr;
    QHash<QString, QPushButton*> m_buttons;
    QString m_currentId;
    int m_railWidth = 168;
};

#endif // DESKTOP_PET_NAVIGATION_WIDGET_H
