#pragma once
#include "capability_types.h"
#include <functional>
struct CapabilityBinding {
    QJsonObject configuration, connections, resultAdapter;
};
class CapabilityExecutor {
public:
    static Result<QJsonObject,DomainError> remoteSchema(const LoadedCapability& loaded,const CapabilityBinding& binding,int timeoutMs);
    static CapabilityOutcome execute(const LoadedCapability& loaded,const CapabilityCall& call,const CapabilityBinding& binding,
                                      const std::function<Result<void,DomainError>(bool)>& beforeDispatch);
    static Result<QJsonValue,DomainError> pointer(const QJsonValue& value,const QString& pointer);
};
