//
// Created by Inoriac on 2025/10/15.
//

#include "mainwindow.h"
#include <QApplication>
#include <QMessageBox>
#include <QFileDialog>
#include <QStandardPaths>
#include <QIcon>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent){
    setWindowTitle("Desktop 3D Pet");
    setMinimumSize(600, 500);
    resize(800, 600);

    createActions();
    createMenus();
    createStatusBar();
    createCentralWidget();
    setupConnections();

    setWindowIcon(QIcon(":/assets/icons/icon.png"));
}

MainWindow::~MainWindow() {

}

void MainWindow::createActions() {
    exitAction = new QAction("退出", this);
    exitAction->setShortcut(QKeySequence::Quit);
    exitAction->setStatusTip("退出应用程序");
    connect(exitAction, &QAction::triggered, this, &MainWindow::close);

    preferencesAction = new QAction("偏好设置", this);
    preferencesAction->setStatusTip("打开偏好设置");
    connect(preferencesAction, &QAction::triggered, this, &MainWindow::OnSettingsChanged);

    aboutAction = new QAction("关于", this);
    aboutAction->setStatusTip("关于 Desktop 3D Pet");
    connect(aboutAction, &QAction::triggered, this, &MainWindow::OnAbout);
}

void MainWindow::createMenus() {
    fileMenu = menuBar()->addMenu(tr("文件(&F)"));
    fileMenu->addAction(exitAction);

    settingsMenu = menuBar()->addMenu(tr("设置(&S)"));
    settingsMenu->addAction(preferencesAction);

    helpMenu = menuBar()->addMenu(tr("帮助(&H)"));
    helpMenu->addAction(aboutAction);
}

void MainWindow::createStatusBar() {
    statusLabel = new QLabel("就绪");
    statusBar()->addWidget(statusLabel);
}

void MainWindow::createCentralWidget() {
    centralWidget = new QWidget();
    setCentralWidget(centralWidget);

    mainLayout = new QVBoxLayout(centralWidget);

    // 创建宠物选择区域
    characterSelectionGroup = new QGroupBox("Character Selection");
    QVBoxLayout *characterLayout = new QVBoxLayout(characterSelectionGroup);

    // TODO：此处显示桌宠名字 后期需要根据用户传入模型来进行修改
    petListWidget = new QListWidget;
    petListWidget->addItem("Milltina");
    petListWidget->setCurrentRow(0);

    QHBoxLayout *buttonLayout = new QHBoxLayout;
    startPetButton = new QPushButton("Start");
    stopPetButton = new QPushButton("Stop");
    stopPetButton->setEnabled(false);

    buttonLayout->addWidget(startPetButton);
    buttonLayout->addWidget(stopPetButton);
    buttonLayout->addStretch();

    characterLayout->addWidget(petListWidget);
    characterLayout->addLayout(buttonLayout);

    // 创建设置区域
    settingsGroup = new QGroupBox("设置");
    QVBoxLayout *settingsLayout = new QVBoxLayout(settingsGroup);

    // 大小设置
    QHBoxLayout * sizeLayout = new QHBoxLayout;
    sizeLabel = new QLabel("大小：");
    sizeSlider = new QSlider(Qt::Horizontal);
    sizeSlider->setRange(50, 200);
    sizeSlider->setValue(100);
    sizeSpinBox = new QSpinBox;
    sizeSpinBox->setRange(50, 200);
    sizeSpinBox->setValue(100);
    sizeSpinBox->setSuffix("%");

    sizeLayout->addWidget(sizeLabel);
    sizeLayout->addWidget(sizeSlider);
    sizeLayout->addWidget(sizeSpinBox);

    // 选项设置
    alwaysOnTopCheckBox = new QCheckBox("Always on top");
    alwaysOnTopCheckBox->setChecked(true);

    clickThroughCheckBox = new QCheckBox("Click through");
    clickThroughCheckBox->setChecked(true);

    soundEnabledCheckBox = new QCheckBox("Sound enabled");
    soundEnabledCheckBox->setChecked(false);

    // 音量设置
    QHBoxLayout *volumeLayout = new QHBoxLayout;
    volumeLabel = new QLabel("Volume");
    volumeSlider = new QSlider(Qt::Horizontal);
    volumeSlider->setRange(0, 100);
    volumeSlider->setValue(75);
    QLabel *volumeValueLabel = new QLabel("80%");

    volumeLayout->addWidget(volumeLabel);
    volumeLayout->addWidget(volumeSlider);
    volumeLayout->addWidget(volumeValueLabel);

    settingsLayout->addLayout(sizeLayout);
    settingsLayout->addWidget(alwaysOnTopCheckBox);
    settingsLayout->addWidget(clickThroughCheckBox);
    settingsLayout->addWidget(soundEnabledCheckBox);
    settingsLayout->addLayout(volumeLayout);

    // 添加到主布局
    mainLayout->addWidget(characterSelectionGroup);
    mainLayout->addWidget(settingsGroup);
    mainLayout->addStretch();
}

void MainWindow::setupConnections() {
    connect(petListWidget, &QListWidget::currentRowChanged, this, &MainWindow::OnPetSelected);
    connect(startPetButton, &QPushButton::clicked, this, &MainWindow::OnStartPet);
    connect(stopPetButton, &QPushButton::clicked, this, &MainWindow::OnStopPet);

    connect(sizeSlider, &QSlider::valueChanged, sizeSpinBox, &QSpinBox::setValue);
    connect(sizeSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), sizeSlider, &QSlider::setValue);

    connect(alwaysOnTopCheckBox, &QCheckBox::toggled, this, &MainWindow::OnSettingsChanged);
    connect(clickThroughCheckBox, &QCheckBox::toggled, this, &MainWindow::OnSettingsChanged);
    connect(soundEnabledCheckBox, &QCheckBox::toggled, this, &MainWindow::OnSettingsChanged);
    connect(volumeSlider, &QSlider::valueChanged, this, &MainWindow::OnSettingsChanged);
}

void MainWindow::OnPetSelected() {
    QString petName = petListWidget->currentItem()->text();
    statusLabel->setText(QString("已选择:%1").arg(petName));
}

void MainWindow::OnStartPet() {
    QString petName = petListWidget->currentItem()->text();
    statusLabel->setText(QString("正在启动 %1...").arg(petName));

    startPetButton->setEnabled(false);
    stopPetButton->setEnabled(true);

    // TODO: 这里将来会调用宠物控制器来启动宠物
    QMessageBox::information(this, "提示", QString("宠物 %1 已启动！").arg(petName));
    statusLabel->setText(QString("%1 正在运行").arg(petName));
}

void MainWindow::OnStopPet() {
    QString petName = petListWidget->currentItem()->text();
    statusLabel->setText(QString("正在停止 %1...").arg(petName));

    startPetButton->setEnabled(true);
    stopPetButton->setEnabled(false);

    // TODO: 这里将来会调用宠物控制器来停止宠物
    QMessageBox::information(this, "提示", QString("宠物 %1 已停止！").arg(petName));
    statusLabel->setText("就绪");
}

void MainWindow::OnSettingsChanged() {
    statusLabel->setText("设置已更改");
}

void MainWindow::OnAbout() {
    QMessageBox::about(this, "关于 Desktop Pet",
                      "<h2>Desktop Pet</h2>"
                      "<p>一个可爱的桌面宠物应用程序</p>"
                      "<p>版本: 1.0.0</p>"
                      "<p>使用 Qt6 开发</p>");
}