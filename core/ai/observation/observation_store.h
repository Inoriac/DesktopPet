#pragma once
#include "ai/chat/conversation_store.h"
#include "observation_types.h"
#include <QSqlDatabase>
class RuntimeUnitOfWork;
struct PendingObservationCue {
  QString conversationId;
  QStringList eventIds;
  QJsonObject watermark;
};
// Value store; each operation owns its connection. Both query and stream use
// it.
class ObservationStore {
public:
  ObservationStore(QString profile, QString path)
      : m_profile(std::move(profile)), m_path(std::move(path)) {}
  Result<ObservationAcknowledgement, DomainError>
  ingest(const ObservationBinding &, const ObservationEnvelope &);
  Result<QString, DomainError> cursor(const ObservationBinding &binding) const;
  static Result<std::optional<PendingObservationCue>, DomainError>
  pendingCue(QSqlDatabase, const QString &profile);
  static Result<void, DomainError> consumeCues(QSqlDatabase,
                                               const QString &profile,
                                               const QStringList &eventIds);
  static Result<QList<ObservationEnvelope>, DomainError>
  normalize(const ObservationAdapter &, const ObservationBinding &,
            const CapabilityOutcome &);

private:
  QString m_profile, m_path;
};
