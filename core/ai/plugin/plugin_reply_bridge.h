#pragma once
#include "plugin_runtime.h"
struct PreparedCapabilityReply {
  RoutedResponse response;
  CapabilityReplyBinding binding;
};
Result<std::optional<PreparedCapabilityReply>, DomainError>
prepareCapabilityReply(const CapabilityCall &, const QJsonObject &configuration,
                       const QString &path);
DeliveryReceipt capabilityDeliveryReceipt(const CapabilityOutcome &,
                                          const PreparedCapabilityReply &);
