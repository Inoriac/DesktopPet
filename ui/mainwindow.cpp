//
// Created by Inoriac on 2025/10/15.
//

#include "mainwindow.h"
#include "render_viewport.h"
#include "pet.h"
#include "configLoader/config_manager.h"

#include <QApplication>
#include <QMessageBox>
#include <QFileDialog>
#include <QStandardPaths>
#include <QIcon>
#include <QInputDialog>
#include <QLineEdit>
#include <QSignalBlocker>

#include "statistic_manager.h"

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent){
    setWindowTitle("Desktop 3D Pet");
    setMinimumSize(600, 500);
    resize(800, 600);

    createActions();
    createMenus();
    createStatusBar();
    createCentralWidget();
    setupConnections();

    Pet::instance().load();

    setWindowIcon(QIcon(":/assets/icons/icon.png"));
}

MainWindow::~MainWindow() {
    if (!activePetName.isEmpty()) {
        StatisticManager::getInstance().recordPetStop(activePetName);
    }

    if(activePetWindow){
        activePetWindow->close();
        delete activePetWindow;
    }
}

void MainWindow::loadPetList(){
    petListWidget->clear();

    // 获取并加载所有 pet 名称
    auto petNames = Pet::instance().getPetNames();

    for (const QString& name : petNames) {
        petListWidget->addItem(name);
    }

    // 默认选择第一个
    if (petListWidget->count() > 0) {
        petListWidget->setCurrentRow(0);
    }
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

    petListWidget = new QListWidget;
    petListWidget->addItem("milltina");
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
    clickThroughCheckBox->setChecked(false);

    aiEnabledCheckBox = new QCheckBox("AI enabled");
    aiEnabledCheckBox->setChecked(ConfigManager::instance().getLlmConfig().enabled);

    soundEnabledCheckBox = new QCheckBox("Sound enabled");
    soundEnabledCheckBox->setChecked(false);

    const VoiceConfig& defaultVoice = ConfigManager::instance().getVoiceConfig();
    voiceEnabledCheckBox = new QCheckBox("Voice synthesis (GENIE / Python)");
    voiceEnabledCheckBox->setChecked(defaultVoice.enabled);

    QHBoxLayout* voiceSpeakerLayout = new QHBoxLayout;
    QLabel* voiceSpeakerLabel = new QLabel("语音角色:");
    voiceSpeakerComboBox = new QComboBox;
    voiceSpeakerComboBox->addItem("Feibi / 菲比（中文）", "feibi");
    voiceSpeakerComboBox->addItem("Mika / 聖園ミカ（日语）", "mika");
    voiceSpeakerComboBox->addItem("ThirtySeven / 37（英语）", "thirtyseven");
    voiceSpeakerComboBox->addItem("自定义角色（按配置加载）", "custom");
    const QString selectedVoice = defaultVoice.speakerMode == "custom"
        ? QStringLiteral("custom")
        : defaultVoice.selectedSpeaker;
    const int selectedVoiceIndex = voiceSpeakerComboBox->findData(selectedVoice);
    voiceSpeakerComboBox->setCurrentIndex(selectedVoiceIndex >= 0 ? selectedVoiceIndex : 0);
    voiceSpeakerComboBox->setEnabled(defaultVoice.enabled);
    voiceSpeakerLayout->addWidget(voiceSpeakerLabel);
    voiceSpeakerLayout->addWidget(voiceSpeakerComboBox);

    const ScreenChatConfig& defaultScreenChat = ConfigManager::instance().getScreenChatConfig();
    autoScreenChatCheckBox = new QCheckBox("Auto screen chat (8-12 min)");
    autoScreenChatCheckBox->setChecked(defaultScreenChat.enabled);

    QHBoxLayout* bubbleOpacityLayout = new QHBoxLayout;
    QLabel* bubbleOpacityLabel = new QLabel("气泡透明度:");
    bubbleOpacitySlider = new QSlider(Qt::Horizontal);
    bubbleOpacitySlider->setRange(10, 100);
    bubbleOpacitySlider->setValue(defaultScreenChat.bubbleOpacityPercent);
    bubbleOpacitySpinBox = new QSpinBox;
    bubbleOpacitySpinBox->setRange(10, 100);
    bubbleOpacitySpinBox->setValue(defaultScreenChat.bubbleOpacityPercent);
    bubbleOpacitySpinBox->setSuffix("%");
    bubbleOpacityLayout->addWidget(bubbleOpacityLabel);
    bubbleOpacityLayout->addWidget(bubbleOpacitySlider);
    bubbleOpacityLayout->addWidget(bubbleOpacitySpinBox);

    QHBoxLayout* bubbleFontLayout = new QHBoxLayout;
    QLabel* bubbleFontLabel = new QLabel("气泡字号:");
    bubbleFontSizeSpinBox = new QSpinBox;
    bubbleFontSizeSpinBox->setRange(10, 36);
    bubbleFontSizeSpinBox->setValue(defaultScreenChat.bubbleFontSize);
    bubbleFontSizeSpinBox->setSuffix("px");
    bubbleFontLayout->addWidget(bubbleFontLabel);
    bubbleFontLayout->addWidget(bubbleFontSizeSpinBox);
    bubbleFontLayout->addStretch();

    QHBoxLayout* bubbleOffsetLayout = new QHBoxLayout;
    QLabel* bubbleOffsetLabel = new QLabel("气泡位置偏移(X/Y):");
    bubbleOffsetXSpinBox = new QSpinBox;
    bubbleOffsetXSpinBox->setRange(-300, 300);
    bubbleOffsetXSpinBox->setValue(defaultScreenChat.bubbleOffsetX);
    bubbleOffsetYSpinBox = new QSpinBox;
    bubbleOffsetYSpinBox->setRange(-300, 300);
    bubbleOffsetYSpinBox->setValue(defaultScreenChat.bubbleOffsetY);
    bubbleOffsetLayout->addWidget(bubbleOffsetLabel);
    bubbleOffsetLayout->addWidget(bubbleOffsetXSpinBox);
    bubbleOffsetLayout->addWidget(bubbleOffsetYSpinBox);

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
    settingsLayout->addWidget(aiEnabledCheckBox);
    settingsLayout->addWidget(autoScreenChatCheckBox);
    settingsLayout->addLayout(bubbleOpacityLayout);
    settingsLayout->addLayout(bubbleFontLayout);
    settingsLayout->addLayout(bubbleOffsetLayout);
    settingsLayout->addWidget(soundEnabledCheckBox);
    settingsLayout->addWidget(voiceEnabledCheckBox);
    settingsLayout->addLayout(voiceSpeakerLayout);
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
    connect(bubbleOpacitySlider, &QSlider::valueChanged, bubbleOpacitySpinBox, &QSpinBox::setValue);
    connect(bubbleOpacitySpinBox, QOverload<int>::of(&QSpinBox::valueChanged), bubbleOpacitySlider, &QSlider::setValue);

    connect(alwaysOnTopCheckBox, &QCheckBox::toggled, this, &MainWindow::OnSettingsChanged);
    connect(clickThroughCheckBox, &QCheckBox::toggled, this, &MainWindow::OnSettingsChanged);
    connect(aiEnabledCheckBox, &QCheckBox::toggled, this, &MainWindow::OnSettingsChanged);
    connect(autoScreenChatCheckBox, &QCheckBox::toggled, this, &MainWindow::OnSettingsChanged);
    connect(voiceEnabledCheckBox, &QCheckBox::toggled, this, [this](bool enabled) {
        if (voiceSpeakerComboBox) {
            voiceSpeakerComboBox->setEnabled(enabled);
        }
        OnSettingsChanged();
    });
    connect(voiceSpeakerComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::OnSettingsChanged);
    connect(bubbleOpacitySlider, &QSlider::valueChanged, this, &MainWindow::OnBubbleAppearanceChanged);
    connect(bubbleFontSizeSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::OnBubbleAppearanceChanged);
    connect(bubbleOffsetXSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::OnBubbleAppearanceChanged);
    connect(bubbleOffsetYSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::OnBubbleAppearanceChanged);
    connect(soundEnabledCheckBox, &QCheckBox::toggled, this, &MainWindow::OnSettingsChanged);
    connect(volumeSlider, &QSlider::valueChanged, this, &MainWindow::OnSettingsChanged);
}

void MainWindow::OnPetSelected() {
    QString petName = petListWidget->currentItem()->text();
    statusLabel->setText(QString("已选择:%1").arg(petName));
}

void MainWindow::OnStartPet() {
    if (activePetWindow) {
        QMessageBox::warning(this, "提示", "已有宠物正在运行，请先停止当前宠物");
        return;
    }

    QListWidgetItem* item = petListWidget->currentItem();
    if (!item) {
        QMessageBox::warning(this, "提示", "请先选择一个桌宠!");
        return;
    }

    QString petName = item->text();
    qDebug() << "Selected pet:" << petName;

    QString modelPath = Pet::instance().getModelPath(petName);
    qDebug() << "Model path:" << modelPath;

    if(modelPath.isEmpty()){
        QMessageBox::warning(this, "提示", "无法找到宠物模型");
        return;
    }

    statusLabel->setText(QString("正在启动 %1...").arg(petName));

    activePetWindow = new PetWindow(petName, nullptr);
    activePetName = petName;

    // 记录桌宠启动统计
    StatisticManager::getInstance().recordPetStart(activePetName);

    // 用于接收宠物窗口关闭信号
    connect(activePetWindow, &PetWindow::requestStop, this, &MainWindow::OnStopPet);

    int sizePercent = sizeSlider->value();
    bool alwaysOnTop = alwaysOnTopCheckBox->isChecked();
    bool clickThrough = clickThroughCheckBox->isChecked();
    bool aiEnabled = aiEnabledCheckBox->isChecked();
    const ScreenChatConfig screenChatConfig = buildScreenChatConfigFromUi();
    const VoiceConfig voiceConfig = buildVoiceConfigFromUi();
    ConfigManager::instance().setLlmEnabled(aiEnabled);
    ConfigManager::instance().setVoiceConfig(voiceConfig);
    activePetWindow->applySettings(sizePercent, alwaysOnTop, clickThrough, aiEnabled, screenChatConfig, voiceConfig);

    activePetWindow->show();

    startPetButton->setEnabled(false);
    stopPetButton->setEnabled(true);
    setWindowFlagControlsLocked(true);

    // QMessageBox::information(this, "提示", QString("宠物 %1 已启动！").arg(petName));
    statusLabel->setText(QString("%1 正在运行").arg(petName));
}

void MainWindow::OnStopPet() {
    QString petName = !activePetName.isEmpty()
                        ? activePetName
                        : (petListWidget->currentItem() ? petListWidget->currentItem()->text() : QString(""));

    if (petName.isEmpty()) {
        statusLabel->setText("就绪");
        return;
    }

    statusLabel->setText(QString("正在停止 %1...").arg(petName));

    // 记录桌宠停止统计
    StatisticManager::getInstance().recordPetStop(petName);

    if (activePetWindow) {
        activePetWindow->close();
        activePetWindow->deleteLater();
        activePetWindow = nullptr;
    }
    activePetName.clear();

    startPetButton->setEnabled(true);
    stopPetButton->setEnabled(false);
    setWindowFlagControlsLocked(false);

    // QMessageBox::information(this, "提示", QString("宠物 %1 已停止！").arg(petName));
    statusLabel->setText("就绪");
}

void MainWindow::OnSettingsChanged() {
    statusLabel->setText("设置已更改");

    // 应用至桌宠界面
    bool aiEnabled = aiEnabledCheckBox->isChecked();
    const VoiceConfig voiceConfig = buildVoiceConfigFromUi();
    ConfigManager::instance().setLlmEnabled(aiEnabled);
    ConfigManager::instance().setVoiceConfig(voiceConfig);

    if(activePetWindow){
        int sizePercent = sizeSlider->value();
        const ScreenChatConfig screenChatConfig = buildScreenChatConfigFromUi();
        activePetWindow->applyRuntimeSettings(sizePercent, aiEnabled, screenChatConfig, voiceConfig);
    }
}

void MainWindow::OnBubbleAppearanceChanged() {
    OnSettingsChanged();
    if (activePetWindow) {
        activePetWindow->previewBubble();
    }
}

void MainWindow::setWindowFlagControlsLocked(bool locked) {
    if (alwaysOnTopCheckBox) {
        alwaysOnTopCheckBox->setEnabled(!locked);
    }
    if (clickThroughCheckBox) {
        clickThroughCheckBox->setEnabled(!locked);
    }
}

ScreenChatConfig MainWindow::buildScreenChatConfigFromUi() const {
    ScreenChatConfig cfg = ConfigManager::instance().getScreenChatConfig();
    cfg.enabled = autoScreenChatCheckBox ? autoScreenChatCheckBox->isChecked() : cfg.enabled;
    cfg.bubbleOpacityPercent = bubbleOpacitySlider ? bubbleOpacitySlider->value() : cfg.bubbleOpacityPercent;
    cfg.bubbleFontSize = bubbleFontSizeSpinBox ? bubbleFontSizeSpinBox->value() : cfg.bubbleFontSize;
    cfg.bubbleOffsetX = bubbleOffsetXSpinBox ? bubbleOffsetXSpinBox->value() : cfg.bubbleOffsetX;
    cfg.bubbleOffsetY = bubbleOffsetYSpinBox ? bubbleOffsetYSpinBox->value() : cfg.bubbleOffsetY;
    cfg.minIntervalMs = 8 * 60 * 1000;
    cfg.maxIntervalMs = 12 * 60 * 1000;
    cfg.petGender = "female";
    return cfg;
}

VoiceConfig MainWindow::buildVoiceConfigFromUi() const {
    VoiceConfig cfg = ConfigManager::instance().getVoiceConfig();
    cfg.enabled = voiceEnabledCheckBox ? voiceEnabledCheckBox->isChecked() : cfg.enabled;
    cfg.preloadOnStart = cfg.enabled;

    const QString selected = voiceSpeakerComboBox
        ? voiceSpeakerComboBox->currentData().toString().trimmed().toLower()
        : cfg.selectedSpeaker;
    if (selected == "custom") {
        cfg.speakerMode = "custom";
    } else if (selected == "feibi" || selected == "mika" || selected == "thirtyseven") {
        cfg.speakerMode = "predefined";
        cfg.selectedSpeaker = selected;
    }

    return cfg;
}

void MainWindow::OnAbout() {
    QMessageBox::about(this, "关于 Desktop Pet",
                      "<h2>Desktop Pet</h2>"
                      "<p>一个可爱的桌面宠物应用程序</p>"
                      "<p>版本: 1.0.3</p>"
                      "<p>使用 Qt6 开发</p>");
}

void MainWindow::OnAddPet() {
    QString petName = QInputDialog::getText(this, "添加桌宠", "请输入桌宠名称:");
    if (petName.isEmpty()) return;

    if (Pet::instance().hasPet(petName)) {
        QMessageBox::warning(this, "错误", "桌宠名称已存在!");
        return;
    }

    QString modelPath = QFileDialog::getOpenFileName(this, "选择模型文件",
        "assets/models/", "GLTF Files (*.gltf *.glb)");
    if (modelPath.isEmpty()) return;

    Pet::instance().addPet(petName, modelPath);
    loadPetList();
}