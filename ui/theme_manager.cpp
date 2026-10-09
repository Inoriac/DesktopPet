#include "theme_manager.h"

#include <QApplication>
#include <QPalette>
#include <QSettings>

ThemeManager& ThemeManager::instance() {
    static ThemeManager manager;
    return manager;
}

ThemeManager::ThemeManager(QObject* parent)
    : QObject(parent) {
    loadPersistedTheme();
}

ThemeManager::Theme ThemeManager::currentTheme() const {
    return m_theme;
}

bool ThemeManager::isDarkTheme() const {
    return m_theme == Theme::Dark;
}

void ThemeManager::setTheme(Theme theme) {
    if (m_theme == theme) {
        return;
    }
    m_theme = theme;
    persistTheme();
    if (auto* app = qobject_cast<QApplication*>(QApplication::instance())) {
        applyTo(app);
    }
    emit themeChanged(m_theme);
}

void ThemeManager::toggleTheme() {
    setTheme(isDarkTheme() ? Theme::Light : Theme::Dark);
}

void ThemeManager::applyTo(QApplication* app) {
    if (!app) {
        return;
    }
    QPalette palette = app->palette();
    if (isDarkTheme()) {
        palette.setColor(QPalette::Window, QColor(QStringLiteral("#0f1117")));
        palette.setColor(QPalette::Base, QColor(QStringLiteral("#111827")));
        palette.setColor(QPalette::Text, QColor(QStringLiteral("#edf2f7")));
        palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#edf2f7")));
    } else {
        palette.setColor(QPalette::Window, QColor(QStringLiteral("#f4f7fb")));
        palette.setColor(QPalette::Base, QColor(QStringLiteral("#ffffff")));
        palette.setColor(QPalette::Text, QColor(QStringLiteral("#202124")));
        palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#202124")));
    }
    app->setPalette(palette);
    app->setStyleSheet(styleSheet());
}

QString ThemeManager::styleSheet() const {
    return isDarkTheme() ? darkStyleSheet() : lightStyleSheet();
}

void ThemeManager::loadPersistedTheme() {
    const QString value = QSettings().value(QStringLiteral("ui/theme"), QStringLiteral("light")).toString().trimmed().toLower();
    m_theme = value == QStringLiteral("dark") ? Theme::Dark : Theme::Light;
}

void ThemeManager::persistTheme() const {
    QSettings().setValue(QStringLiteral("ui/theme"), isDarkTheme() ? QStringLiteral("dark") : QStringLiteral("light"));
}

QString ThemeManager::lightStyleSheet() const {
    return QStringLiteral(R"qss(
* {
    font-family: "Segoe UI", "Microsoft YaHei UI", "Microsoft YaHei";
    font-size: 14px;
}
QMainWindow {
    background: #f4f7fb;
    color: #202124;
}
QMenuBar {
    background: #f4f7fb;
    color: #202124;
    padding: 4px;
    border: none;
    border-bottom: 1px solid rgba(32, 33, 36, 0.08);
}
QMenu {
    background: #ffffff;
    color: #202124;
    border: 1px solid rgba(32, 33, 36, 0.10);
    border-radius: 10px;
    padding: 6px;
}
QMenu::item {
    padding: 7px 22px;
    border-radius: 8px;
}
QMenuBar::item:selected, QMenu::item:selected {
    background: rgba(0, 102, 204, 0.10);
    border-radius: 8px;
}
QStatusBar {
    background: #f4f7fb;
    color: #667085;
    border-top: 1px solid rgba(32, 33, 36, 0.08);
}
QStatusBar QLabel {
    color: #667085;
    background: transparent;
}
QPushButton {
    background: #ffffff;
    color: #202124;
    border: 1px solid rgba(32, 33, 36, 0.12);
    border-radius: 12px;
    padding: 9px 16px;
    font-weight: 600;
}
QPushButton:hover {
    background: #f1f6ff;
    border-color: rgba(0, 102, 204, 0.28);
}
QPushButton:pressed {
    background: #e6f0ff;
}
QPushButton:disabled {
    color: #98a2b3;
    background: #eef2f6;
    border-color: rgba(32, 33, 36, 0.08);
}
QComboBox, QSpinBox, QLineEdit {
    background: #ffffff;
    color: #202124;
    border: 1px solid rgba(32, 33, 36, 0.14);
    border-radius: 10px;
    padding: 7px 12px;
    min-height: 24px;
}
QComboBox {
    padding-right: 34px;
}
QComboBox::drop-down {
    width: 30px;
    border: none;
    border-top-right-radius: 10px;
    border-bottom-right-radius: 10px;
    background: transparent;
    subcontrol-origin: padding;
    subcontrol-position: top right;
}
QComboBox::down-arrow {
    image: none;
    width: 10px;
    height: 6px;
    margin-right: 9px;
}
QComboBox QAbstractItemView {
    background: #ffffff;
    color: #202124;
    border: 1px solid rgba(32, 33, 36, 0.10);
    border-radius: 10px;
    padding: 6px;
    selection-background-color: rgba(0, 102, 204, 0.12);
    selection-color: #005fb8;
    outline: 0;
}
QSpinBox {
    padding-right: 28px;
}
QSpinBox::up-button, QSpinBox::down-button {
    width: 24px;
    border: none;
    background: transparent;
    subcontrol-origin: border;
}
QSpinBox::up-button {
    subcontrol-position: top right;
    border-top-right-radius: 10px;
}
QSpinBox::down-button {
    subcontrol-position: bottom right;
    border-bottom-right-radius: 10px;
}
QSpinBox::up-arrow {
    image: url(assets/icons/spin_up_light.svg);
    width: 8px;
    height: 5px;
}
QSpinBox::down-arrow {
    image: url(assets/icons/spin_down_light.svg);
    width: 8px;
    height: 5px;
}
QComboBox:hover, QSpinBox:hover, QLineEdit:hover {
    border-color: rgba(0, 102, 204, 0.40);
}
QComboBox:focus, QSpinBox:focus, QLineEdit:focus {
    border-color: #0066cc;
}
QListWidget {
    background: transparent;
    border: none;
    outline: 0;
}
QListWidget::item {
    padding: 12px 14px;
    border-radius: 12px;
    margin: 3px 0;
}
QListWidget::item:hover {
    background: rgba(0, 102, 204, 0.08);
}
QListWidget::item:selected {
    background: rgba(0, 102, 204, 0.16);
    color: #005fb8;
}
QSlider::groove:horizontal {
    min-height: 24px;
    height: 5px;
    background: #d9e2ef;
    border-radius: 3px;
}
QSlider::sub-page:horizontal {
    background: #0066cc;
    border-radius: 3px;
}
QSlider::handle:horizontal {
    width: 13px;
    height: 13px;
    margin: -5px 0;
    border-radius: 8px;
    background: #f8fafc;
    border: 2px solid #005fb8;
}
QSlider::handle:horizontal:hover {
    background: #ffffff;
    border: 2px solid #003f7d;
}
QScrollBar:vertical {
    background: transparent;
    width: 12px;
    border: none;
}
QScrollBar::handle:vertical {
    background: rgba(52, 64, 84, 0.30);
    border-radius: 6px;
    min-height: 34px;
}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
    height: 0;
    background: transparent;
    border: none;
}
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
    background: transparent;
}
)qss");
}

QString ThemeManager::darkStyleSheet() const {
    return QStringLiteral(R"qss(
* {
    font-family: "Segoe UI", "Microsoft YaHei UI", "Microsoft YaHei";
    font-size: 14px;
}
QMainWindow {
    background: #0f1117;
    color: #edf2f7;
}
QMenuBar {
    background: #0f1117;
    color: #d8dee9;
    padding: 4px;
    border: none;
    border-bottom: 1px solid rgba(255, 255, 255, 0.07);
}
QMenu {
    background: #171c27;
    color: #edf2f7;
    border: 1px solid rgba(255, 255, 255, 0.08);
    border-radius: 10px;
    padding: 6px;
}
QMenu::item {
    padding: 7px 22px;
    border-radius: 8px;
}
QMenuBar::item:selected, QMenu::item:selected {
    background: rgba(96, 165, 250, 0.14);
    border-radius: 8px;
}
QStatusBar {
    background: #0f1117;
    color: #9aa4b2;
    border-top: 1px solid rgba(255, 255, 255, 0.07);
}
QStatusBar QLabel {
    color: #9aa4b2;
    background: transparent;
}
QPushButton {
    background: #1d2330;
    color: #edf2f7;
    border: 1px solid rgba(255, 255, 255, 0.09);
    border-radius: 12px;
    padding: 9px 16px;
    font-weight: 600;
}
QPushButton:hover {
    background: #252d3d;
    border-color: rgba(96, 165, 250, 0.34);
}
QPushButton:pressed {
    background: #2e3748;
}
QPushButton:disabled {
    color: #687283;
    background: #161a23;
    border-color: rgba(255, 255, 255, 0.05);
}
QComboBox, QSpinBox, QLineEdit {
    background: #111827;
    color: #edf2f7;
    border: 1px solid rgba(255, 255, 255, 0.10);
    border-radius: 10px;
    padding: 7px 12px;
    min-height: 24px;
}
QComboBox {
    padding-right: 34px;
}
QComboBox::drop-down {
    width: 30px;
    border: none;
    border-top-right-radius: 10px;
    border-bottom-right-radius: 10px;
    background: transparent;
    subcontrol-origin: padding;
    subcontrol-position: top right;
}
QComboBox::down-arrow {
    image: none;
    width: 10px;
    height: 6px;
    margin-right: 9px;
}
QComboBox QAbstractItemView {
    background: #171c27;
    color: #edf2f7;
    border: 1px solid rgba(255, 255, 255, 0.08);
    border-radius: 10px;
    padding: 6px;
    selection-background-color: rgba(96, 165, 250, 0.16);
    selection-color: #93c5fd;
    outline: 0;
}
QSpinBox {
    padding-right: 28px;
}
QSpinBox::up-button, QSpinBox::down-button {
    width: 24px;
    border: none;
    background: transparent;
    subcontrol-origin: border;
}
QSpinBox::up-button {
    subcontrol-position: top right;
    border-top-right-radius: 10px;
}
QSpinBox::down-button {
    subcontrol-position: bottom right;
    border-bottom-right-radius: 10px;
}
QSpinBox::up-arrow {
    image: url(assets/icons/spin_up_dark.svg);
    width: 8px;
    height: 5px;
}
QSpinBox::down-arrow {
    image: url(assets/icons/spin_down_dark.svg);
    width: 8px;
    height: 5px;
}
QComboBox:hover, QSpinBox:hover, QLineEdit:hover {
    border-color: rgba(96, 165, 250, 0.42);
}
QComboBox:focus, QSpinBox:focus, QLineEdit:focus {
    border-color: #60a5fa;
}
QListWidget {
    background: transparent;
    border: none;
    outline: 0;
}
QListWidget::item {
    padding: 12px 14px;
    border-radius: 12px;
    margin: 3px 0;
}
QListWidget::item:hover {
    background: rgba(96, 165, 250, 0.10);
}
QListWidget::item:selected {
    background: rgba(96, 165, 250, 0.18);
    color: #93c5fd;
}
QSlider::groove:horizontal {
    min-height: 24px;
    height: 5px;
    background: #2b3344;
    border-radius: 3px;
}
QSlider::sub-page:horizontal {
    background: #60a5fa;
    border-radius: 3px;
}
QSlider::handle:horizontal {
    width: 13px;
    height: 13px;
    margin: -5px 0;
    border-radius: 8px;
    background: #111827;
    border: 2px solid #93c5fd;
}
QSlider::handle:horizontal:hover {
    background: #0b1120;
    border: 2px solid #dbeafe;
}
QScrollBar:vertical {
    background: transparent;
    width: 12px;
    border: none;
}
QScrollBar::handle:vertical {
    background: rgba(203, 213, 225, 0.30);
    border-radius: 6px;
    min-height: 34px;
}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
    height: 0;
    background: transparent;
    border: none;
}
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
    background: transparent;
}
)qss");
}
