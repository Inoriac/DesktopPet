#include "capability_types.h"
QJsonObject CapabilityRef::toJson() const {
    QJsonObject result{{"pluginId",pluginId},{"entryId",entryId},{"entryVersion",entryVersion}};
    if (!operationId.isEmpty()) result["operationId"] = operationId;
    return result;
}
CapabilityRef CapabilityRef::fromJson(const QJsonObject& value) {
    return {value["pluginId"].toString(), value["entryId"].toString(), value["entryVersion"].toString(), value["operationId"].toString()};
}
QJsonObject LoadedCapability::toPublicJson() const {
    QJsonObject result{{"ref",ref.toJson()},{"kind",kind},{"instructions",instructions},
        {"catalogRevision",qint64(catalogRevision)},{"definitionDigest",definitionDigest}};
    if (kind == "skill") result["dependencies"] = dependencies;
    else if (ref.operationId.isEmpty()) result["operations"] = operations;
    else {
        result["parameters"] = parameterSchema;
        result["result"] = operation["result"];
        result["effects"] = operation["effects"];
        result["description"] = operation["description"];
    }
    return result;
}
QJsonObject CapabilityOutcome::toJson() const {
    QJsonObject result{{"callId",callId},{"activityId",activityId},{"stage",stage},
        {"effectCertainty",effectCertainty},{"profileId",context.profileId},{"conversationId",context.conversationId},{"catalogRevision",qint64(catalogRevision)},{"data",data},{"receipt",receipt},{"tracking",tracking},{"executed",executed}};
    if (!error.code.isEmpty()) result["error"] = QJsonObject{{"code",error.code},{"message",error.message}};
    return result;
}
