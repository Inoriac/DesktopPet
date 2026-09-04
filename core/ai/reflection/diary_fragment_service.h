#ifndef DIARY_FRAGMENT_SERVICE_H
#define DIARY_FRAGMENT_SERVICE_H

#include <QDateTime>
#include <QObject>
#include <QTimer>
#include <atomic>
#include <functional>
#include <memory>

#include "reflection_types.h"

class ModelRouter;
class ContextAssembler;
class PrivateKeyProvider;
class PrivatePsycheCrypto;
class SqlitePrivatePsycheRepository;
class EventLedger;
class AgentRuntimeServices;

class DiaryFragmentService : public QObject {
    Q_OBJECT
public:
    struct Policy {
        int idleWindowMinutes;
        int minEventsSinceLastFragment;
        int maxFragmentBodyChars;
    };

    DiaryFragmentService(
        QString profileId,
        AgentRuntimeServices* services,
        ModelRouter* modelRouter,
        ContextAssembler* contextAssembler,
        PrivateKeyProvider* keyProvider,
        PrivatePsycheCrypto* crypto,
        SqlitePrivatePsycheRepository* repository,
        EventLedger* eventLedger,
        Policy policy = {15, 3, 400});
    ~DiaryFragmentService() override;

    // 探针：系统空闲秒数 / 大脑忙碌状态（未设置时不自动收集）
    void setIdleProbe(std::function<int()> userIdleSeconds);
    void setBusyProbe(std::function<bool()> isBrainBusy);

    // 启动周期性便签收集（间隔 = idleWindowMinutes），并在启动后延迟检测
    // 孤儿 Draft（有便签无日记的历史日期），发现后发 recoveryNeeded。
    void start();
    void stop();

    Result<void, DomainError> collectFragmentIfIdle();
    Result<QList<DiaryFragment>, DomainError> draftsForDate(const QDate& localDate);
    Result<QList<QPair<QDate, int>>, DomainError> detectOrphanDrafts() const;

signals:
    void fragmentCollected(const QString& fragmentId, const QDate& localDate);
    // 存在孤儿便签的历史日期（最老优先），需补写日记
    void recoveryNeeded(const QDate& localDate);

private:
    QString m_profileId;
    AgentRuntimeServices* m_services;
    ModelRouter* m_modelRouter;
    ContextAssembler* m_contextAssembler;
    PrivateKeyProvider* m_keyProvider;
    PrivatePsycheCrypto* m_crypto;
    SqlitePrivatePsycheRepository* m_repository;
    EventLedger* m_eventLedger;
    Policy m_policy;
    QTimer m_timer;
    std::function<int()> m_userIdleSeconds;
    std::function<bool()> m_isBrainBusy;
    bool m_started = false;
    std::shared_ptr<std::atomic_bool> m_alive =
        std::make_shared<std::atomic_bool>(true);

    Result<QString, DomainError> composeFragment(
        const QDate& localDate,
        int segmentIndex,
        qint64 fromSequence,
        qint64 toSequence,
        const QList<EventRecord>& events);
    Result<DiaryFragment, DomainError> decryptFragment(
        const EncryptedDiaryFragment& encrypted) const;
};

#endif // DIARY_FRAGMENT_SERVICE_H
