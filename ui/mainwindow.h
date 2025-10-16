//
// Created by Inoriac on 2025/10/15.
//

#ifndef DESKTOP_PET_MAINWINDOW_H
#define DESKTOP_PET_MAINWINDOW_H

#include <QMainWindow>
#include <QMenuBar>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QListWidget>
#include <QGroupBox>
#include <QSlider>
#include <QSpinBox>
#include <QCheckBox>
#include <QComboBox>


QT_BEGIN_NAMESPACE
class QAction;
class QMenu;
class RenderViewport;

class MainWindow : public QMainWindow{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void OnPetSelected();
    void OnStartPet();
    void OnStopPet();
    void OnSettingsChanged();
    void OnAbout();

private:
    void createActions();
    void createMenus();
    void createStatusBar();
    void createCentralWidget();
    void setupConnections();

    // UI组件
    QWidget *centralWidget;
    QVBoxLayout *mainLayout;

    // 宠物选择区域
    QGroupBox *characterSelectionGroup;
    QListWidget *petListWidget;
    QPushButton *startPetButton;
    QPushButton *stopPetButton;

    // 设置区域
    QGroupBox *settingsGroup;
    QLabel *sizeLabel;
    QSlider *sizeSlider;
    QSpinBox *sizeSpinBox;
    QCheckBox *alwaysOnTopCheckBox;
    QCheckBox *clickThroughCheckBox;
    QCheckBox *soundEnabledCheckBox;
    QSlider *volumeSlider;
    QLabel *volumeLabel;

    // 菜单和动作
    QMenu *fileMenu;
    QMenu *settingsMenu;
    QMenu *helpMenu;
    QAction *exitAction;
    QAction *preferencesAction;
    QAction *aboutAction;

    // 状态栏
    QLabel *statusLabel;

    RenderViewport *renderViewport;
};


#endif //DESKTOP_PET_MAINWINDOW_H