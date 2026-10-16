#pragma once
#include "observation_store.h"
#include <QObject>
#include <QThread>
#include <mutex>
struct SourceLaunch {
  ObservationBinding binding;
  LoadedObservationSource source;
  QString program;
  QStringList arguments, secrets;
  quint64 generation = 0;
  std::function<Result<void, DomainError>()> validate;
};
Result<SourceLaunch, DomainError>
prepareSourceLaunch(const ObservationBinding &, const LoadedObservationSource &,
                    const QJsonObject &configuration);
class ObservationService final : public QObject {
  Q_OBJECT
public:
  ObservationService(QString profile, QString path, QObject *parent = nullptr);
  ~ObservationService() override;
  void ingestObservation(ObservationBinding binding,
                         ObservationEnvelope observation,
                         ObservationCompletion completion);
  void setObservationSource(SourceLaunch launch, bool enabled,
                            SourceCompletion completion = {});
  void stopSources();
signals:
  void messageAccepted(const AcceptedMessage &message);
  void sourceStateChanged(const SourceState &state);

private:
  struct Worker;
  void post(std::function<void(Worker *)>);
  QThread *m_thread = nullptr;
  Worker *m_worker = nullptr;
  std::mutex m_mutex;
};
