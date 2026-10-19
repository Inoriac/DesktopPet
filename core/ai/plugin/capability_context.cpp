#include "capability_context.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QSet>

namespace {
struct Material {
    QString name;
    QJsonObject data;
    bool success = false;
};
QString definitionKey(const QJsonObject& data) {
    auto identity = data["ref"].toObject();
    identity.remove("operationId");
    identity["catalogRevision"] = data["catalogRevision"];
    identity["definitionDigest"] = data["definitionDigest"];
    return QString::fromUtf8(QJsonDocument(identity).toJson(QJsonDocument::Compact));
}
bool completeDefinition(const QJsonObject& data) {
    const auto reference = data["ref"].toObject();
    if (reference["pluginId"].toString().isEmpty() || reference["entryId"].toString().isEmpty()
        || reference["entryVersion"].toString().isEmpty()
        || (!data["instructions"].isString() && !data["instructionsRef"].isString()))
        return false;
    if (data["kind"] == "skill") return data["dependencies"].isArray();
    if (data["kind"] != "tool") return false;
    if (reference["operationId"].toString().isEmpty()) return data["operations"].isArray();
    return data["parameters"].isObject() && data["effects"].isObject() && data["result"].isObject();
}
QList<Material> messagesToMaterials(const QList<ChatMessage>& messages) {
    QList<Material> result;
    for (const auto& message : messages) {
        if (message.role != "tool" || !CapabilityContext::isMetadataTool(message.name)) {
            result.append(Material{});
            continue;
        }
        const auto envelope = QJsonDocument::fromJson(message.content.toUtf8()).object();
        result.append({message.name, envelope["data"].toObject(), envelope["success"].toBool()});
    }
    return result;
}
QList<Material> stepsToMaterials(const QJsonArray& steps) {
    QList<Material> result;
    for (const auto& item : steps) {
        const auto step = item.toObject();
        result.append({step["name"].toString(), step["output"].toObject(), step["status"] == "succeeded"});
    }
    return result;
}
QList<Material> project(QList<Material> materials) {
    QString currentTool, currentSkill;
    QSet<QString> operations;
    QSet<int> retained;
    int overview = -1;
    int latestDiscovery = -1;
    for (int i = materials.size() - 1; i >= 0; --i) {
        const auto& item = materials[i];
        if (!item.success) continue;
        if (item.name == "discover_capabilities" && item.data["entries"].isArray()) {
            if (latestDiscovery < 0) latestDiscovery = i;
            continue;
        }
        if (item.name != "load_capability" || !completeDefinition(item.data)) continue;
        const auto key = definitionKey(item.data);
        if (item.data["kind"] == "skill") {
            if (currentSkill.isEmpty()) { currentSkill = key; retained.insert(i); }
            continue;
        }
        if (currentTool.isEmpty()) currentTool = key;
        if (key != currentTool) continue;
        const auto operation = item.data["ref"].toObject()["operationId"].toString();
        if (operation.isEmpty()) {
            if (overview < 0) overview = i;
        } else if (operations.size() < 2 && !operations.contains(operation)) {
            operations.insert(operation);
            retained.insert(i);
        }
    }
    if (operations.isEmpty() && overview >= 0) retained.insert(overview);
    QSet<QString> instructionBodies;
    for (int i = 0; i < materials.size(); ++i) {
        auto& item = materials[i];
        if (!item.success) continue;
        if (item.name == "discover_capabilities" && item.data["entries"].isArray()) {
            if (i != latestDiscovery)
                item.data = {{"contextState", "discovery_completed"},
                    {"entryCount", item.data["entries"].toArray().size()},
                    {"message", "Previous discovery completed. Discover again for current summaries."}};
            continue;
        }
        if (item.name != "load_capability" || !completeDefinition(item.data)) continue;
        if (!retained.contains(i)) {
            item.data = {{"ref", item.data["ref"]}, {"kind", item.data["kind"]},
                {"catalogRevision", item.data["catalogRevision"]}, {"definitionDigest", item.data["definitionDigest"]},
                {"contextState", "reload_required"},
                {"message", "Definition released from this request. Call load_capability before invoking this operation."}};
            continue;
        }
        const auto bodyId = QString::fromLatin1(QCryptographicHash::hash(
            definitionKey(item.data).toUtf8(), QCryptographicHash::Sha256).toHex());
        // Only reference a body actually present in this request.
        if (instructionBodies.contains(bodyId)) {
            item.data.remove("instructions");
            item.data.remove("instructionsId");
            item.data["instructionsRef"] = bodyId;
        } else if (item.data["instructions"].isString()) {
            item.data.remove("instructionsRef");
            item.data["instructionsId"] = bodyId;
            instructionBodies.insert(bodyId);
        }
    }
    return materials;
}
bool containsOperation(const QList<Material>& materials, const QJsonObject& reference) {
    if (reference["operationId"].toString().isEmpty()) return false;
    QSet<QString> bodies;
    for (const auto& item : materials)
        if (item.success && item.name == "load_capability" && completeDefinition(item.data)
            && item.data["instructions"].isString())
            bodies.insert(item.data["instructionsId"].toString());
    for (const auto& item : materials) {
        if (!item.success || item.name != "load_capability" || !completeDefinition(item.data)
            || item.data["kind"] != "tool" || item.data["ref"].toObject() != reference) continue;
        if (item.data["instructions"].isString()) return true;
        const auto body = item.data["instructionsRef"].toString();
        if (!body.isEmpty() && bodies.contains(body)) return true;
    }
    return false;
}
}

bool CapabilityContext::isMetadataTool(const QString& name) {
    return name == "discover_capabilities" || name == "load_capability";
}
QList<ChatMessage> CapabilityContext::projectMessages(const QList<ChatMessage>& messages) {
    auto result = messages;
    const auto original = messagesToMaterials(messages);
    const auto projected = project(original);
    for (int i = 0; i < result.size(); ++i) {
        if (projected[i].data == original[i].data) continue;
        auto envelope = QJsonDocument::fromJson(result[i].content.toUtf8()).object();
        envelope["data"] = projected[i].data;
        result[i].content = QString::fromUtf8(QJsonDocument(envelope).toJson(QJsonDocument::Compact));
    }
    return result;
}
QJsonArray CapabilityContext::projectSteps(const QJsonArray& steps) {
    auto result = steps;
    const auto original = stepsToMaterials(steps);
    const auto projected = project(original);
    for (int i = 0; i < result.size(); ++i) {
        if (projected[i].data == original[i].data) continue;
        auto step = result[i].toObject();
        step["output"] = projected[i].data;
        result[i] = step;
    }
    return result;
}
bool CapabilityContext::hasOperation(const QList<ChatMessage>& projected, const QJsonObject& reference) {
    return containsOperation(messagesToMaterials(projected), reference);
}
bool CapabilityContext::hasOperation(const QJsonArray& projectedSteps, const QJsonObject& reference) {
    return containsOperation(stepsToMaterials(projectedSteps), reference);
}
