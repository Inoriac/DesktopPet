#pragma once
#include "ai/chat/conversation_types.h"
#include "ai/plugin/capability_types.h"

struct ObservationBinding {
  QString id, pluginId, channel, connectionKey, provider, nameSpace;
  ExternalIdentity selfIdentity;
  QList<HostConversationBinding> conversations;
  QJsonObject source;
};
struct ObservationAdapter {
  CapabilityRef ref;
  QString bindingId, itemsPointer;
  QJsonObject fields;
};
enum class ObservationTransport { Source, Query };
struct ObservationEnvelope {
  QString bindingId;
  QJsonObject payload;
  quint64 generation = 0;
  ObservationTransport transport = ObservationTransport::Source;
};
struct ObservationAcknowledgement {
  QString eventId, cursor;
  bool committed = false, duplicate = false;
  AcceptedMessage message;
};
using ObservationCompletion =
    std::function<void(Result<ObservationAcknowledgement, DomainError>)>;
struct SourceControl {
  QString bindingId;
  bool enabled = false;
  QJsonObject configuration;
  quint64 expectedGeneration = 0;
};
struct SourceState {
  QString bindingId, status, error, cursor;
  quint64 generation = 0;
};
using SourceCompletion = std::function<void(Result<SourceState, DomainError>)>;
struct ObservationConfiguration {
  QHash<QString, ObservationBinding> bindings;
  QList<ObservationAdapter> adapters;
  static Result<ObservationConfiguration, DomainError>
  parse(const QJsonObject &config);
};
struct LoadedObservationSource {
  QString pluginId, id, rootPath, digest;
  quint64 catalogRevision = 0;
  QJsonObject definition;
};
