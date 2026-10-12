#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <functional>
#include <optional>

// All identifiers and capabilities come from the avatar configuration. This
// contract deliberately uses QtCore only; rendering SDKs belong in adapters.
enum class AvatarRequestSource { Model, User, Emotion, Automatic, Debug };

class AvatarBackend : public QObject {
    Q_OBJECT
  public:
    using QObject::QObject;
    virtual QJsonObject resources() const = 0;
    virtual bool start(const QString& state, const QString& action, bool loop, quint64 generation,
                       QString* error) = 0;
    virtual void stop(const QString& initialState) = 0;
    virtual void setDrawing(bool enabled) = 0;
    virtual bool moveTo(const QPoint& target, QPoint* actual) = 0;
  signals:
    void cycleFinished(quint64 generation, quint64 cycle);
    void playbackFailed(quint64 generation, QString reason);
};

class AvatarPresentation : public QObject {
    Q_OBJECT
  public:
    explicit AvatarPresentation(QObject* parent = nullptr);
    ~AvatarPresentation() override;
    QStringList configure(const QJsonObject& config);
    void setBackend(AvatarBackend* backend);
    void setPolicyProvider(std::function<QJsonObject()> provider);
    QJsonObject capabilities() const;
    QJsonObject snapshot() const;
    QJsonObject candidates() const;
    QJsonObject request(const QString& target, AvatarRequestSource source = AvatarRequestSource::Model,
                        int ttlMs = 0, const QString& reason = {}, const QString& requestId = {});
    QJsonObject requestSemantic(const QString& semantic, AvatarRequestSource source, int ttlMs = 0);
    QJsonObject interact(const QString& kind, const QString& region = {});
    QJsonObject requestResult(const QString& id) const;
    QJsonObject cancel(const QString& id);
    void setVisible(bool visible);
    QJsonObject moveTo(const QPoint& position);
    bool hasSemanticState(const QString& semantic) const;

  signals:
    void requestChanged(QJsonObject result);
    void stateChanged(QJsonObject snapshot);
    void interactionOccurred(QString kind, QString region);
    void visibilityChanged(bool visible);
    void cycleCompleted(QString state, quint64 cycle);

  private:
    struct Pending {
        QString id, target, reason, requestedTarget;
        AvatarRequestSource source;
        qint64 deadline = 0;
        QStringList path;
        bool cancelPending = false;
    };
    template <typename F> auto serial(F&& fn) const -> decltype(fn()) {
        using Result = decltype(fn());
        if (QThread::currentThread() == thread())
            return fn();
        Result result{};
        QMetaObject::invokeMethod(
            const_cast<AvatarPresentation*>(this), [&] { result = fn(); }, Qt::BlockingQueuedConnection);
        return result;
    }
    QJsonObject state(const QString& name) const;
    QJsonObject transition(const QString& from, const QString& to) const;
    QJsonArray actions(const QString& name) const;
    QString rejection(const Pending& request, bool checkPath = true) const;
    QString interruption() const;
    bool startState(const QString& name, QString* error);
    void begin(Pending pending);
    void advance();
    void onCycle(quint64 generation, quint64 cycle);
    void expire();
    void drain();
    void resetPlayback();
    void finish(const Pending& pending, const QString& status, const QString& reason = {});
    QJsonObject report(const Pending& pending, const QString& status, const QString& reason = {});
    void publish();
    void terminatePending(const QString& status, const QString& reason);
    static int priority(AvatarRequestSource source);
    QJsonObject m_config;
    QPointer<AvatarBackend> m_backend;
    std::function<QJsonObject()> m_policy;
    QString m_current, m_action, m_error;
    bool m_visible = false, m_playing = false, m_atBoundary = false;
    quint64 m_generation = 0, m_cycle = 0, m_revision = 0;
    qint64 m_playbackDeadline = 0;
    QList<Pending> m_pending;
    std::optional<Pending> m_active;
    QHash<QString, QJsonObject> m_results;
    QStringList m_resultOrder;
    QTimer m_expiryTimer;
};
