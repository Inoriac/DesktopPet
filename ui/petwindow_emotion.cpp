#include "petwindow.h"

#include "behavior/behavior_manager.h"
#include "configLoader/config_manager.h"
#include "controller/pet_controller.h"
#include "emotion/emotion_engine.h"
#include "emotion/sqlite_emotion_state_repository.h"
#include "render_engine.h"
#include "render_viewport.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QTimer>

namespace {

QString emotionDatabasePath(const QString& modelName) {
    const QByteArray digest = QCryptographicHash::hash(
        modelName.trimmed().toCaseFolded().toUtf8(), QCryptographicHash::Sha256).toHex().left(20);
    return QDir(QStringLiteral("runtime/emotion"))
        .filePath(QString::fromLatin1(digest) + QStringLiteral(".db"));
}

QString emotionDisplayName(EmotionType emotion) {
    switch (emotion) {
    case EmotionType::Neutral:
        return QStringLiteral("平静");
    case EmotionType::Joy:
        return QStringLiteral("愉快");
    case EmotionType::Sadness:
        return QStringLiteral("低落");
    case EmotionType::Anger:
        return QStringLiteral("生气");
    case EmotionType::Fear:
        return QStringLiteral("不安");
    case EmotionType::Surprise:
        return QStringLiteral("惊讶");
    }
    return QStringLiteral("平静");
}

} // namespace

void PetWindow::setupEmotionSystem() {
    const EmotionConfig config = ConfigManager::instance().getEmotionConfig();
    emotionRepository = std::make_unique<SQLiteEmotionStateRepository>(emotionDatabasePath(modelName));
    emotionEngine = std::make_unique<EmotionEngine>(config, emotionRepository.get());
    petController = std::make_unique<PetController>(emotionEngine.get());
    behaviorManager = std::make_unique<BehaviorManager>(config.expressionQueueLimit);

    behaviorManager->setCurrentStateProvider([this]() -> QString {
        if (!renderViewport || !renderViewport->getRenderEngine()) {
            return {};
        }
        const auto* player = renderViewport->getRenderEngine()->getAnimationPlayer();
        return player ? QString::fromStdString(player->getCurrentStateName()) : QString{};
    });
    behaviorManager->setAnimationPlayer([this](const QString& state) -> bool {
        if (!renderViewport || !renderViewport->getRenderEngine() || !renderViewport->getAnimationManager()) {
            return false;
        }
        auto* player = renderViewport->getRenderEngine()->getAnimationPlayer();
        if (!player || player->getCurrentStateName() != "Idle") {
            return false;
        }

        const AnimationStateMachineDefinition& stateMachine =
            renderViewport->getAnimationManager()->getStateMachine();
        const auto stateIt = stateMachine.stateIndexMap.find(state.toStdString());
        if (stateIt == stateMachine.stateIndexMap.end()) {
            return false;
        }
        const int index = stateIt->second;
        if (index < 0
            || index >= static_cast<int>(stateMachine.states.size())
            || stateMachine.states[index].clipOptions.empty()) {
            return false;
        }

        player->changeState(state.toStdString());
        return player->getCurrentStateName() == state.toStdString();
    });

    connect(emotionEngine.get(), &EmotionEngine::expressionRequested,
            this, [this](const ExpressionRequest& request) {
        if (behaviorManager) {
            behaviorManager->handleExpression(request, QDateTime::currentDateTimeUtc());
        }
    });
    connect(emotionEngine.get(), &EmotionEngine::persistenceFailed,
            this, [this](const QString& operation) {
        qWarning() << "[Emotion] persistence failed:" << operation
                   << (emotionRepository ? emotionRepository->lastError() : QString{});
    });
    connect(behaviorManager.get(), &BehaviorManager::expressionDropped,
            this, [](const ExpressionRequest& request, const QString& reason) {
        qDebug() << "[Emotion] expression dropped:"
                 << emotionTypeToString(request.emotion) << reason;
    });

    emotionEngine->restore(QDateTime::currentDateTimeUtc());

    emotionTickTimer = new QTimer(this);
    emotionTickTimer->setInterval(5000);
    connect(emotionTickTimer, &QTimer::timeout, this, [this]() {
        if (emotionEngine) {
            emotionEngine->advanceTo(QDateTime::currentDateTimeUtc());
        }
    });
    if (emotionEngine->isEnabled()) {
        emotionTickTimer->start();
    }

    emotionBehaviorTimer = new QTimer(this);
    emotionBehaviorTimer->setInterval(250);
    connect(emotionBehaviorTimer, &QTimer::timeout, this, [this]() {
        if (behaviorManager) {
            behaviorManager->processPending(QDateTime::currentDateTimeUtc());
        }
    });
    emotionBehaviorTimer->start();
}

QString PetWindow::emotionStatusText() const {
    if (!emotionEngine || !emotionEngine->isEnabled()) {
        return QStringLiteral("当前状态：已关闭");
    }
    const EmotionSnapshot state = emotionEngine->snapshot(QDateTime::currentDateTimeUtc());
    return QStringLiteral("当前状态：%1 %2%  心境 %3 / %4")
        .arg(emotionDisplayName(state.active))
        .arg(qRound(state.intensity * 100.0))
        .arg(state.moodValence, 0, 'f', 2)
        .arg(state.moodArousal, 0, 'f', 2);
}

void PetWindow::setEmotionSystemEnabled(bool enabled) {
    if (!emotionEngine) {
        return;
    }
    emotionEngine->setEnabled(enabled, QDateTime::currentDateTimeUtc());
    if (enabled) {
        if (emotionTickTimer) emotionTickTimer->start();
    } else {
        if (emotionTickTimer) emotionTickTimer->stop();
        if (behaviorManager) behaviorManager->clearPending();
    }
}

void PetWindow::resetEmotionSystem() {
    if (!emotionEngine) {
        return;
    }
    emotionEngine->reset(QDateTime::currentDateTimeUtc());
    if (behaviorManager) {
        behaviorManager->clearPending();
    }
}
