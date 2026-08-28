#include <QtTest>

#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <optional>
#include <memory>

#include "ai/context/context_assembler.h"
#include "ai/llm/llm_chat_model_client.h"
#include "ai/llm/llm_chat_service.h"
#include "ai/model/model_role_registry.h"
#include "ai/model/model_router.h"
#include "configLoader/config_manager.h"

namespace {

ModelRouteConfig route(const QString& id,
                       const QString& model,
                       bool supportsVision = false) {
    ModelRouteConfig config;
    config.routeId = id;
    config.enabled = true;
    config.llm.enabled = true;
    config.llm.provider = QStringLiteral("openai-compatible");
    config.llm.baseUrl = QStringLiteral("https://models.example/v1");
    config.llm.apiKey = QStringLiteral("test-key");
    config.llm.model = model;
    config.llm.timeoutMs = 1000;
    config.supportsVision = supportsVision;
    return config;
}

ModelRoleConfig roleConfig(ModelRole role,
                           const QList<ModelRouteConfig>& routes) {
    ModelRoleConfig config;
    config.role = role;
    config.routes = routes;
    return config;
}

LlmResponse responseWithContent(const QString& content) {
    LlmResponse response;
    response.content = content;
    return response;
}

class FakeModelCompletionClient final : public ModelCompletionClient {
public:
    struct Reply {
        bool success = false;
        LlmResponse response;
        QString error;
    };

    QList<Reply> replies;
    QList<QString> routeIds;
    QList<ModelRouteConfig> selectedRoutes;
    QList<QList<ChatMessage>> messageBatches;

    void completeOnce(const ModelRouteConfig& selectedRoute,
                      const QList<ChatMessage>& messages,
                      const QJsonArray& tools,
                      LlmCompletionHandler callback,
                      const QString& petName) override {
        Q_UNUSED(tools)
        Q_UNUSED(petName)
        routeIds.append(selectedRoute.routeId);
        selectedRoutes.append(selectedRoute);
        messageBatches.append(messages);
        if (replies.isEmpty()) {
            callback(false, {}, QStringLiteral("missing fake reply"));
            return;
        }
        const Reply reply = replies.takeFirst();
        callback(reply.success, reply.response, reply.error);
    }
};

class CapturingLlmClient final : public LlmClient {
public:
    QList<LlmConfig> configs;

    void sendChatCompletionAsync(const LlmConfig& config,
                                 const QList<ChatMessage>& messages,
                                 const QJsonArray& tools,
                                 LlmCompletionHandler callback) override {
        Q_UNUSED(messages)
        Q_UNUSED(tools)
        configs.append(config);
        callback(true, responseWithContent(QStringLiteral("ok")), {});
    }
};

ChatMessage message(const QString& role, const QString& content) {
    ChatMessage value;
    value.role = role;
    value.content = content;
    return value;
}

ContextProjection projection(ContextPartition partition,
                             const QString& content) {
    ContextProjection value;
    value.partition = partition;
    value.messages = {message(QStringLiteral("system"), content)};
    return value;
}

QJsonObject objectSchema() {
    const QJsonObject answerProperty{
        {QStringLiteral("type"), QStringLiteral("string")}
    };
    return {
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("answer")}},
        {QStringLiteral("properties"), QJsonObject{
             {QStringLiteral("answer"), answerProperty}}}
    };
}

QString writeConfig(QTemporaryDir& directory, const QJsonObject& config) {
    const QString path = directory.filePath(QStringLiteral("model-role-config.json"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return QString();
    }
    file.write(QJsonDocument(config).toJson(QJsonDocument::Compact));
    file.close();
    return path;
}

QJsonObject configWithDialogueRoute(const QJsonObject& route) {
    const QJsonObject dialogueRole{
        {QStringLiteral("routes"), QJsonArray{route}}
    };
    const QJsonObject profile{
        {QStringLiteral("enabled"), true},
        {QStringLiteral("modelRoles"), QJsonObject{
             {QStringLiteral("dialogue"), dialogueRole}}}
    };
    return {
        {QStringLiteral("aiSettings"), QJsonObject{
             {QStringLiteral("activeProfile"), QStringLiteral("default")},
             {QStringLiteral("profiles"), QJsonObject{
                  {QStringLiteral("default"), profile}}}}}
    };
}

} // namespace

class ModelRouterTests : public QObject {
    Q_OBJECT

private slots:
    void resolve_whenPrimaryRouteMeetsConstraints_shouldReturnPrimary();
    void resolve_whenPrimaryIsOpenCircuit_shouldReturnConfiguredFallback();
    void resolve_whenNoRouteSupportsVision_shouldReturnRoleUnavailable();

    void completeAsync_whenPrimarySucceeds_shouldReturnValidatedResponseAndRoleDimensions();
    void completeAsync_whenOutputSchemaInvalidOnce_shouldRepairThenReturnValidResponse();
    void completeAsync_whenProviderTimesOut_shouldTryFallbackWithoutChangingContextScope();
    void requestVisionSummary_whenVisionRoleUsesIndependentEndpoint_shouldRouteWithoutGlobalCredentials();

    void assemble_whenDialogueRole_shouldIncludePersonaMemoryAndSkillSummary();
    void assemble_whenDialogueRole_shouldExcludeDiaryInnerThoughtAndOwnerAccess();
    void assemble_whenDiaryRole_shouldExposeOnlyDiaryProjectionWithinBudget();
    void assemble_whenRequestedPartitionIsForbidden_shouldReturnScopeDenied();
    void assemble_whenDaydreamRequestsEvidenceAndMemory_shouldReturnOnlyAllowedPartitions();
    void assemble_whenDaydreamRequestsPersona_shouldRejectScope();

    void getModelRoleConfig_whenRoleConfigured_shouldReturnRoleSpecificModel();
    void getModelRoleConfig_whenOnlyLegacyConfigExists_shouldMapItToDialogue();
    void getModelRoleConfig_whenRoleReferencesAnthropicEndpoint_shouldMergeConnectionAndModel();
    void getModelRoleConfig_whenEndpointReferenceIsMissing_shouldRejectRouteWithoutDefaultKeyFallback();
    void getModelRoleConfig_whenHeaderValueIsNotString_shouldIgnoreItWithoutPuttingItInExtraParams();
    void getModelRoleConfig_whenDaydreamReferencesSpecialEndpoint_shouldUseOnlyItsKeyAndModel();
    void getModelRoleConfig_whenLegacyDaydreamFieldsExist_shouldSynthesizeCompatibleRoute();
    void configFor_whenRoleExists_shouldReturnOnlyRequestedRole();
    void completeOnce_whenRouteIsValid_shouldDisableServiceLevelRetries();
};

void ModelRouterTests::resolve_whenPrimaryRouteMeetsConstraints_shouldReturnPrimary() {
    ModelRoleRegistry registry({roleConfig(
        ModelRole::Dialogue,
        {route(QStringLiteral("primary"), QStringLiteral("model-a")),
         route(QStringLiteral("fallback"), QStringLiteral("model-b"))})});
    FakeModelCompletionClient client;
    ModelRouter router(&registry, &client);

    const auto result = router.resolve(ModelRole::Dialogue, {});

    QVERIFY(result.isOk());
    QCOMPARE(result.value().routeId, QStringLiteral("primary"));
}

void ModelRouterTests::resolve_whenPrimaryIsOpenCircuit_shouldReturnConfiguredFallback() {
    ModelRoleRegistry registry({roleConfig(
        ModelRole::Dialogue,
        {route(QStringLiteral("primary"), QStringLiteral("model-a")),
         route(QStringLiteral("fallback"), QStringLiteral("model-b"))})});
    FakeModelCompletionClient client;
    client.replies = {
        {false, {}, QStringLiteral("network timeout")},
        {true, responseWithContent(QStringLiteral("ok")), {}}
    };
    ModelRouter router(&registry, &client);
    ModelRequest request;
    request.role = ModelRole::Dialogue;
    request.messages = {message(QStringLiteral("user"), QStringLiteral("hello"))};
    std::optional<Result<ModelCompletion, DomainError>> completion;

    router.completeAsync(request, [&](Result<ModelCompletion, DomainError> result) {
        completion.emplace(std::move(result));
    });
    const auto resolved = router.resolve(ModelRole::Dialogue, {});

    QVERIFY(completion.has_value());
    QVERIFY(completion->isOk());
    QVERIFY(resolved.isOk());
    QCOMPARE(resolved.value().routeId, QStringLiteral("fallback"));
}

void ModelRouterTests::resolve_whenNoRouteSupportsVision_shouldReturnRoleUnavailable() {
    ModelRoleRegistry registry({roleConfig(
        ModelRole::Vision,
        {route(QStringLiteral("text-only"), QStringLiteral("model-a"), false)})});
    FakeModelCompletionClient client;
    ModelRouter router(&registry, &client);
    ModelConstraints constraints;
    constraints.requiresVision = true;

    const auto result = router.resolve(ModelRole::Vision, constraints);

    QVERIFY(!result.isOk());
    QCOMPARE(result.error().code, QStringLiteral("MODEL_ROLE_UNAVAILABLE"));
}

void ModelRouterTests::completeAsync_whenPrimarySucceeds_shouldReturnValidatedResponseAndRoleDimensions() {
    ModelRoleRegistry registry({roleConfig(
        ModelRole::Dialogue,
        {route(QStringLiteral("dialogue-primary"), QStringLiteral("model-a"))})});
    FakeModelCompletionClient client;
    client.replies = {{true, responseWithContent(QStringLiteral("{\"answer\":\"ok\"}")), {}}};
    ModelRouter router(&registry, &client);
    ModelRequest request;
    request.role = ModelRole::Dialogue;
    request.responseSchema = objectSchema();
    std::optional<Result<ModelCompletion, DomainError>> result;

    router.completeAsync(request, [&](Result<ModelCompletion, DomainError> value) {
        result.emplace(std::move(value));
    });

    QVERIFY(result.has_value());
    QVERIFY(result->isOk());
    QCOMPARE(result->value().response.content, QStringLiteral("{\"answer\":\"ok\"}"));
    QCOMPARE(result->value().dimensions.role, ModelRole::Dialogue);
    QCOMPARE(result->value().dimensions.provider, QStringLiteral("openai-compatible"));
    QCOMPARE(result->value().dimensions.model, QStringLiteral("model-a"));
    QCOMPARE(result->value().dimensions.routeId, QStringLiteral("dialogue-primary"));
    QVERIFY(!result->value().fallbackUsed);
}

void ModelRouterTests::completeAsync_whenOutputSchemaInvalidOnce_shouldRepairThenReturnValidResponse() {
    ModelRoleRegistry registry({roleConfig(
        ModelRole::FastExtract,
        {route(QStringLiteral("extract-primary"), QStringLiteral("model-a"))})});
    FakeModelCompletionClient client;
    client.replies = {
        {true, responseWithContent(QStringLiteral("not-json")), {}},
        {true, responseWithContent(QStringLiteral("{\"answer\":\"fixed\"}")), {}}
    };
    ModelRouter router(&registry, &client);
    ModelRequest request;
    request.role = ModelRole::FastExtract;
    request.responseSchema = objectSchema();
    request.messages = {message(QStringLiteral("user"), QStringLiteral("extract"))};
    std::optional<Result<ModelCompletion, DomainError>> result;

    router.completeAsync(request, [&](Result<ModelCompletion, DomainError> value) {
        result.emplace(std::move(value));
    });

    QVERIFY(result.has_value());
    QVERIFY(result->isOk());
    QCOMPARE(client.routeIds, QList<QString>({QStringLiteral("extract-primary"),
                                              QStringLiteral("extract-primary")}));
    QCOMPARE(client.messageBatches.at(1).size(), request.messages.size() + 1);
    QVERIFY(client.messageBatches.at(1).last().content.contains(QStringLiteral("JSON")));
}

void ModelRouterTests::completeAsync_whenProviderTimesOut_shouldTryFallbackWithoutChangingContextScope() {
    ModelRoleRegistry registry({roleConfig(
        ModelRole::Diary,
        {route(QStringLiteral("diary-primary"), QStringLiteral("model-a")),
         route(QStringLiteral("diary-fallback"), QStringLiteral("model-b"))})});
    FakeModelCompletionClient client;
    client.replies = {
        {false, {}, QStringLiteral("provider timeout")},
        {true, responseWithContent(QStringLiteral("done")), {}}
    };
    ModelRouter router(&registry, &client);
    ModelRequest request;
    request.role = ModelRole::Diary;
    request.messages = {message(QStringLiteral("user"), QStringLiteral("diary-projection-ref"))};
    std::optional<Result<ModelCompletion, DomainError>> result;

    router.completeAsync(request, [&](Result<ModelCompletion, DomainError> value) {
        result.emplace(std::move(value));
    });

    QVERIFY(result.has_value());
    QVERIFY(result->isOk());
    QVERIFY(result->value().fallbackUsed);
    QCOMPARE(result->value().dimensions.routeId, QStringLiteral("diary-fallback"));
    QCOMPARE(client.messageBatches.size(), 2);
    QCOMPARE(client.messageBatches.at(0).at(0).content,
             client.messageBatches.at(1).at(0).content);
    QCOMPARE(client.messageBatches.at(1).size(), request.messages.size());
}

void ModelRouterTests::requestVisionSummary_whenVisionRoleUsesIndependentEndpoint_shouldRouteWithoutGlobalCredentials() {
    ModelRouteConfig visionRoute = route(
        QStringLiteral("vision-special"), QStringLiteral("vision-model"), true);
    visionRoute.llm.provider = QStringLiteral("anthropic-messages");
    visionRoute.llm.baseUrl = QStringLiteral("https://vision.example");
    visionRoute.llm.apiKey = QStringLiteral("vision-only-key");
    ModelRoleRegistry registry({roleConfig(ModelRole::Vision, {visionRoute})});
    FakeModelCompletionClient client;
    client.replies = {{true, responseWithContent(
        QStringLiteral("{\"main_content\":\"screen\",\"pet_reply\":\"hello\"}")), {}}};
    ModelRouter router(&registry, &client);
    ChatMessage visionMessage;
    visionMessage.role = QStringLiteral("user");
    visionMessage.contentBlocks = QJsonArray{
        QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                    {QStringLiteral("text"), QStringLiteral("inspect")}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("image")},
                    {QStringLiteral("mediaType"), QStringLiteral("image/png")},
                    {QStringLiteral("data"), QStringLiteral("YWJj")}}
    };
    ModelRequest request;
    request.role = ModelRole::Vision;
    request.constraints.requiresVision = true;
    request.messages = {visionMessage};
    std::optional<Result<ModelCompletion, DomainError>> result;

    router.completeAsync(request, [&](Result<ModelCompletion, DomainError> value) {
        result.emplace(std::move(value));
    });

    QVERIFY(result.has_value());
    QVERIFY(result->isOk());
    QCOMPARE(client.selectedRoutes.size(), 1);
    QCOMPARE(client.selectedRoutes.first().llm.apiKey,
             QStringLiteral("vision-only-key"));
    QCOMPARE(client.selectedRoutes.first().llm.provider,
             QStringLiteral("anthropic-messages"));
    QCOMPARE(client.messageBatches.first().first().contentBlocks.size(), 2);
}

void ModelRouterTests::assemble_whenDialogueRole_shouldIncludePersonaMemoryAndSkillSummary() {
    ContextAssembler assembler;
    ContextRequest request;
    request.requestedPartitions = {
        ContextPartition::CurrentInput,
        ContextPartition::Persona,
        ContextPartition::RelevantMemory,
        ContextPartition::SkillSummary
    };
    request.projections = {
        projection(ContextPartition::CurrentInput, QStringLiteral("input")),
        projection(ContextPartition::Persona, QStringLiteral("persona")),
        projection(ContextPartition::RelevantMemory, QStringLiteral("memory")),
        projection(ContextPartition::SkillSummary, QStringLiteral("skills"))
    };

    const auto result = assembler.assemble(ModelRole::Dialogue, request);

    QVERIFY(result.isOk());
    QCOMPARE(result.value().size(), 4);
    QCOMPARE(result.value().at(0).content, QStringLiteral("input"));
    QCOMPARE(result.value().at(1).content, QStringLiteral("persona"));
    QCOMPARE(result.value().at(2).content, QStringLiteral("memory"));
    QCOMPARE(result.value().at(3).content, QStringLiteral("skills"));
}

void ModelRouterTests::assemble_whenDialogueRole_shouldExcludeDiaryInnerThoughtAndOwnerAccess() {
    ContextAssembler assembler;
    ContextRequest request;
    request.requestedPartitions = {ContextPartition::CurrentInput};
    request.projections = {
        projection(ContextPartition::CurrentInput, QStringLiteral("visible")),
        projection(ContextPartition::DiaryProjection, QStringLiteral("private diary")),
        projection(ContextPartition::InnerThought, QStringLiteral("inner thought")),
        projection(ContextPartition::OwnerAccess, QStringLiteral("owner access"))
    };

    const auto result = assembler.assemble(ModelRole::Dialogue, request);

    QVERIFY(result.isOk());
    QCOMPARE(result.value().size(), 1);
    QCOMPARE(result.value().first().content, QStringLiteral("visible"));
}

void ModelRouterTests::assemble_whenDiaryRole_shouldExposeOnlyDiaryProjectionWithinBudget() {
    ContextAssembler assembler;
    ContextRequest request;
    request.queryBudgetChars = 8;
    request.requestedPartitions = {ContextPartition::DiaryProjection};
    request.projections = {
        projection(ContextPartition::CurrentInput, QStringLiteral("not-visible")),
        projection(ContextPartition::DiaryProjection, QStringLiteral("1234567890"))
    };

    const auto result = assembler.assemble(ModelRole::Diary, request);

    QVERIFY(result.isOk());
    QCOMPARE(result.value().size(), 1);
    QCOMPARE(result.value().first().content, QStringLiteral("12345678"));
}

void ModelRouterTests::assemble_whenRequestedPartitionIsForbidden_shouldReturnScopeDenied() {
    ContextAssembler assembler;
    ContextRequest request;
    request.requestedPartitions = {
        ContextPartition::CurrentInput,
        ContextPartition::OwnerAccess
    };

    const auto result = assembler.assemble(ModelRole::Dialogue, request);

    QVERIFY(!result.isOk());
    QCOMPARE(result.error().code, QStringLiteral("CONTEXT_SCOPE_DENIED"));
}

void ModelRouterTests::assemble_whenDaydreamRequestsEvidenceAndMemory_shouldReturnOnlyAllowedPartitions() {
    ContextAssembler assembler;
    ContextRequest request;
    request.requestedPartitions = {
        ContextPartition::EvidenceWindow,
        ContextPartition::RelevantMemory
    };
    request.projections = {
        projection(ContextPartition::EvidenceWindow, QStringLiteral("batch evidence")),
        projection(ContextPartition::RelevantMemory, QStringLiteral("related memory")),
        projection(ContextPartition::Persona, QStringLiteral("must stay hidden"))
    };

    const auto result = assembler.assemble(ModelRole::Daydream, request);

    QVERIFY(result.isOk());
    QCOMPARE(result.value().size(), 2);
    QCOMPARE(result.value().at(0).content, QStringLiteral("batch evidence"));
    QCOMPARE(result.value().at(1).content, QStringLiteral("related memory"));
}

void ModelRouterTests::assemble_whenDaydreamRequestsPersona_shouldRejectScope() {
    ContextAssembler assembler;
    ContextRequest request;
    request.requestedPartitions = {ContextPartition::Persona};
    request.projections = {
        projection(ContextPartition::Persona, QStringLiteral("must stay hidden"))
    };

    const auto result = assembler.assemble(ModelRole::Daydream, request);

    QVERIFY(!result.isOk());
    QCOMPARE(result.error().code, QStringLiteral("CONTEXT_SCOPE_DENIED"));
}

void ModelRouterTests::getModelRoleConfig_whenRoleConfigured_shouldReturnRoleSpecificModel() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QJsonObject diaryRoute{
        {QStringLiteral("routeId"), QStringLiteral("diary-primary")},
        {QStringLiteral("enabled"), true},
        {QStringLiteral("provider"), QStringLiteral("openai-compatible")},
        {QStringLiteral("baseUrl"), QStringLiteral("https://diary.example/v1")},
        {QStringLiteral("apiKey"), QStringLiteral("diary-key")},
        {QStringLiteral("model"), QStringLiteral("diary-model")}
    };
    const QJsonObject diaryRole{
        {QStringLiteral("routes"), QJsonArray{diaryRoute}}
    };
    const QJsonObject profile{
        {QStringLiteral("enabled"), true},
        {QStringLiteral("provider"), QStringLiteral("legacy")},
        {QStringLiteral("baseUrl"), QStringLiteral("https://legacy.example/v1")},
        {QStringLiteral("apiKey"), QStringLiteral("legacy-key")},
        {QStringLiteral("model"), QStringLiteral("legacy-model")},
        {QStringLiteral("modelRoles"), QJsonObject{
             {QStringLiteral("diary"), diaryRole}}}
    };
    const QJsonObject root{
        {QStringLiteral("aiSettings"), QJsonObject{
             {QStringLiteral("activeProfile"), QStringLiteral("default")},
             {QStringLiteral("profiles"), QJsonObject{
                  {QStringLiteral("default"), profile}}}}}
    };
    const QString path = writeConfig(directory, root);
    QVERIFY(!path.isEmpty());
    QVERIFY(ConfigManager::instance().loadConfig(path));

    const ModelRoleConfig config =
        ConfigManager::instance().getModelRoleConfig(ModelRole::Diary);

    QCOMPARE(config.role, ModelRole::Diary);
    QCOMPARE(config.routes.size(), 1);
    QCOMPARE(config.routes.first().routeId, QStringLiteral("diary-primary"));
    QCOMPARE(config.routes.first().llm.model, QStringLiteral("diary-model"));
}

void ModelRouterTests::getModelRoleConfig_whenOnlyLegacyConfigExists_shouldMapItToDialogue() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QJsonObject root{
        {QStringLiteral("aiSettings"), QJsonObject{
             {QStringLiteral("enabled"), true},
             {QStringLiteral("provider"), QStringLiteral("openai-compatible")},
             {QStringLiteral("baseUrl"), QStringLiteral("https://legacy.example/v1")},
             {QStringLiteral("apiKey"), QStringLiteral("legacy-key")},
             {QStringLiteral("model"), QStringLiteral("legacy-model")}}}};
    const QString path = writeConfig(directory, root);
    QVERIFY(!path.isEmpty());
    QVERIFY(ConfigManager::instance().loadConfig(path));

    const ModelRoleConfig config =
        ConfigManager::instance().getModelRoleConfig(ModelRole::Dialogue);

    QCOMPARE(config.role, ModelRole::Dialogue);
    QCOMPARE(config.routes.size(), 1);
    QCOMPARE(config.routes.first().llm.model, QStringLiteral("legacy-model"));
    QCOMPARE(config.routes.first().llm.apiKey, QStringLiteral("legacy-key"));
}

void ModelRouterTests::getModelRoleConfig_whenRoleReferencesAnthropicEndpoint_shouldMergeConnectionAndModel() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QJsonObject endpoint{
        {QStringLiteral("provider"), QStringLiteral("anthropic-messages")},
        {QStringLiteral("baseUrl"), QStringLiteral("https://text.example")},
        {QStringLiteral("apiKey"), QStringLiteral("endpoint-key")},
        {QStringLiteral("anthropicVersion"), QStringLiteral("2024-01-01")},
        {QStringLiteral("extraHeaders"), QJsonObject{
             {QStringLiteral("x-gateway-tenant"), QStringLiteral("desktop-pet")},
             {QStringLiteral("x-feature"), QStringLiteral("streaming")}}}
    };
    const QJsonObject route{
        {QStringLiteral("routeId"), QStringLiteral("dialogue-primary")},
        {QStringLiteral("endpointRef"), QStringLiteral("DEFAULT")},
        {QStringLiteral("model"), QStringLiteral("role-model")},
        {QStringLiteral("apiKey"), QStringLiteral("must-be-ignored")}
    };
    QJsonObject root = configWithDialogueRoute(route);
    QJsonObject aiSettings = root.value(QStringLiteral("aiSettings")).toObject();
    QJsonObject profiles = aiSettings.value(QStringLiteral("profiles")).toObject();
    QJsonObject profile = profiles.value(QStringLiteral("default")).toObject();
    profile.insert(QStringLiteral("modelEndpoints"), QJsonObject{
        {QStringLiteral("DEFAULT"), endpoint}});
    profiles.insert(QStringLiteral("default"), profile);
    aiSettings.insert(QStringLiteral("profiles"), profiles);
    root.insert(QStringLiteral("aiSettings"), aiSettings);
    const QString path = writeConfig(directory, root);
    QVERIFY(!path.isEmpty());
    QVERIFY(ConfigManager::instance().loadConfig(path));

    const ModelRoleConfig config =
        ConfigManager::instance().getModelRoleConfig(ModelRole::Dialogue);

    QCOMPARE(config.routes.size(), 1);
    QCOMPARE(config.routes.first().llm.provider,
             QStringLiteral("anthropic-messages"));
    QCOMPARE(config.routes.first().llm.apiKey, QStringLiteral("endpoint-key"));
    QCOMPARE(config.routes.first().llm.model, QStringLiteral("role-model"));
    QCOMPARE(config.routes.first().llm.anthropicVersion, QStringLiteral("2024-01-01"));
    QCOMPARE(config.routes.first().llm.extraHeaders.value(
                 QStringLiteral("x-gateway-tenant")).toString(),
             QStringLiteral("desktop-pet"));
    QCOMPARE(config.routes.first().llm.extraHeaders.value(
                 QStringLiteral("x-feature")).toString(),
             QStringLiteral("streaming"));
}

void ModelRouterTests::getModelRoleConfig_whenEndpointReferenceIsMissing_shouldRejectRouteWithoutDefaultKeyFallback() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QJsonObject endpoint{
        {QStringLiteral("provider"), QStringLiteral("anthropic-messages")},
        {QStringLiteral("baseUrl"), QStringLiteral("https://default.example")},
        {QStringLiteral("apiKey"), QStringLiteral("default-key")}
    };
    const QJsonObject route{
        {QStringLiteral("routeId"), QStringLiteral("dialogue-primary")},
        {QStringLiteral("endpointRef"), QStringLiteral("MISSPELLED")},
        {QStringLiteral("model"), QStringLiteral("role-model")},
        {QStringLiteral("provider"), QStringLiteral("openai-compatible")},
        {QStringLiteral("baseUrl"), QStringLiteral("https://inline.example/v1")},
        {QStringLiteral("apiKey"), QStringLiteral("inline-key")}
    };
    QJsonObject root = configWithDialogueRoute(route);
    QJsonObject aiSettings = root.value(QStringLiteral("aiSettings")).toObject();
    QJsonObject profiles = aiSettings.value(QStringLiteral("profiles")).toObject();
    QJsonObject profile = profiles.value(QStringLiteral("default")).toObject();
    profile.insert(QStringLiteral("modelEndpoints"), QJsonObject{
        {QStringLiteral("DEFAULT"), endpoint}});
    profiles.insert(QStringLiteral("default"), profile);
    aiSettings.insert(QStringLiteral("profiles"), profiles);
    root.insert(QStringLiteral("aiSettings"), aiSettings);
    const QString path = writeConfig(directory, root);
    QVERIFY(!path.isEmpty());
    QVERIFY(ConfigManager::instance().loadConfig(path));

    const ModelRoleConfig config =
        ConfigManager::instance().getModelRoleConfig(ModelRole::Dialogue);

    QVERIFY(config.routes.isEmpty());
}

void ModelRouterTests::getModelRoleConfig_whenHeaderValueIsNotString_shouldIgnoreItWithoutPuttingItInExtraParams() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QJsonObject endpoint{
        {QStringLiteral("provider"), QStringLiteral("anthropic-messages")},
        {QStringLiteral("baseUrl"), QStringLiteral("https://text.example")},
        {QStringLiteral("apiKey"), QStringLiteral("text-key")},
        {QStringLiteral("extraHeaders"), QJsonObject{
             {QStringLiteral("x-valid"), QStringLiteral("kept")},
             {QStringLiteral("x-invalid"), 42}}}
    };
    const QJsonObject route{
        {QStringLiteral("routeId"), QStringLiteral("dialogue-primary")},
        {QStringLiteral("endpointRef"), QStringLiteral("DEFAULT")},
        {QStringLiteral("model"), QStringLiteral("claude-text")},
        {QStringLiteral("extraParams"), QJsonObject{
             {QStringLiteral("top_k"), 7}}}
    };
    QJsonObject root = configWithDialogueRoute(route);
    QJsonObject aiSettings = root.value(QStringLiteral("aiSettings")).toObject();
    QJsonObject profiles = aiSettings.value(QStringLiteral("profiles")).toObject();
    QJsonObject profile = profiles.value(QStringLiteral("default")).toObject();
    profile.insert(QStringLiteral("modelEndpoints"), QJsonObject{
        {QStringLiteral("DEFAULT"), endpoint}});
    profiles.insert(QStringLiteral("default"), profile);
    aiSettings.insert(QStringLiteral("profiles"), profiles);
    root.insert(QStringLiteral("aiSettings"), aiSettings);
    const QString path = writeConfig(directory, root);
    QVERIFY(!path.isEmpty());
    QVERIFY(ConfigManager::instance().loadConfig(path));

    const LlmConfig config = ConfigManager::instance()
                                 .getModelRoleConfig(ModelRole::Dialogue)
                                 .routes.first().llm;

    QCOMPARE(config.extraHeaders.size(), 1);
    QCOMPARE(config.extraHeaders.value(QStringLiteral("x-valid")).toString(),
             QStringLiteral("kept"));
    QVERIFY(!config.extraHeaders.contains(QStringLiteral("x-invalid")));
    QCOMPARE(config.extraParams.value(QStringLiteral("top_k")).toInt(), 7);
    QVERIFY(!config.extraParams.contains(QStringLiteral("x-invalid")));
}

void ModelRouterTests::getModelRoleConfig_whenDaydreamReferencesSpecialEndpoint_shouldUseOnlyItsKeyAndModel() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QJsonObject defaultEndpoint{
        {QStringLiteral("provider"), QStringLiteral("anthropic-messages")},
        {QStringLiteral("baseUrl"), QStringLiteral("https://dialogue.example")},
        {QStringLiteral("apiKey"), QStringLiteral("dialogue-key")}
    };
    const QJsonObject daydreamEndpoint{
        {QStringLiteral("provider"), QStringLiteral("openai-compatible")},
        {QStringLiteral("baseUrl"), QStringLiteral("https://daydream.example/v1")},
        {QStringLiteral("apiKey"), QStringLiteral("daydream-key")}
    };
    const QJsonObject daydreamRoute{
        {QStringLiteral("routeId"), QStringLiteral("daydream-primary")},
        {QStringLiteral("endpointRef"), QStringLiteral("DREAM")},
        {QStringLiteral("model"), QStringLiteral("small-daydream-model")},
        {QStringLiteral("maxTokens"), 777},
        {QStringLiteral("temperature"), 0.15}
    };
    const QJsonObject profile{
        {QStringLiteral("enabled"), true},
        {QStringLiteral("modelEndpoints"), QJsonObject{
             {QStringLiteral("DEFAULT"), defaultEndpoint},
             {QStringLiteral("DREAM"), daydreamEndpoint}}},
        {QStringLiteral("modelRoles"), QJsonObject{
             {QStringLiteral("daydream"), QJsonObject{
                  {QStringLiteral("routes"), QJsonArray{daydreamRoute}}}}}}
    };
    const QJsonObject root{{QStringLiteral("aiSettings"), QJsonObject{
        {QStringLiteral("activeProfile"), QStringLiteral("default")},
        {QStringLiteral("profiles"), QJsonObject{
             {QStringLiteral("default"), profile}}}}}};

    const QString path = writeConfig(directory, root);
    QVERIFY(!path.isEmpty());
    QVERIFY(ConfigManager::instance().loadConfig(path));

    const ModelRoleConfig config =
        ConfigManager::instance().getModelRoleConfig(ModelRole::Daydream);

    QCOMPARE(config.role, ModelRole::Daydream);
    QCOMPARE(config.routes.size(), 1);
    const LlmConfig& llm = config.routes.first().llm;
    QCOMPARE(llm.provider, QStringLiteral("openai-compatible"));
    QCOMPARE(llm.baseUrl, QStringLiteral("https://daydream.example/v1"));
    QCOMPARE(llm.apiKey, QStringLiteral("daydream-key"));
    QCOMPARE(llm.model, QStringLiteral("small-daydream-model"));
    QCOMPARE(llm.maxTokens, 777);
    QCOMPARE(llm.temperature, 0.15);
    QVERIFY(llm.apiKey != QLatin1String("dialogue-key"));
}

void ModelRouterTests::getModelRoleConfig_whenLegacyDaydreamFieldsExist_shouldSynthesizeCompatibleRoute() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QJsonObject profile{
        {QStringLiteral("enabled"), true},
        {QStringLiteral("provider"), QStringLiteral("openai-compatible")},
        {QStringLiteral("baseUrl"), QStringLiteral("https://legacy.example/v1")},
        {QStringLiteral("apiKey"), QStringLiteral("legacy-dialogue-key")},
        {QStringLiteral("model"), QStringLiteral("legacy-dialogue-model")},
        {QStringLiteral("daydream"), QJsonObject{
             {QStringLiteral("model"), QString()},
             {QStringLiteral("maxTokens"), 900},
             {QStringLiteral("temperature"), 0.35}}}
    };
    const QJsonObject root{{QStringLiteral("aiSettings"), QJsonObject{
        {QStringLiteral("activeProfile"), QStringLiteral("default")},
        {QStringLiteral("profiles"), QJsonObject{
             {QStringLiteral("default"), profile}}}}}};

    const QString path = writeConfig(directory, root);
    QVERIFY(!path.isEmpty());
    QVERIFY(ConfigManager::instance().loadConfig(path));

    const ModelRoleConfig config =
        ConfigManager::instance().getModelRoleConfig(ModelRole::Daydream);

    QCOMPARE(config.role, ModelRole::Daydream);
    QCOMPARE(config.routes.size(), 1);
    const ModelRouteConfig& route = config.routes.first();
    QCOMPARE(route.routeId, QStringLiteral("daydream-legacy"));
    QCOMPARE(route.llm.provider, QStringLiteral("openai-compatible"));
    QCOMPARE(route.llm.baseUrl, QStringLiteral("https://legacy.example/v1"));
    QCOMPARE(route.llm.apiKey, QStringLiteral("legacy-dialogue-key"));
    QCOMPARE(route.llm.model, QStringLiteral("legacy-dialogue-model"));
    QCOMPARE(route.llm.maxTokens, 900);
    QCOMPARE(route.llm.temperature, 0.35);

    const QJsonObject daydreamOnlyModelProfile{
        {QStringLiteral("enabled"), true},
        {QStringLiteral("provider"), QStringLiteral("openai-compatible")},
        {QStringLiteral("baseUrl"), QStringLiteral("https://legacy.example/v1")},
        {QStringLiteral("apiKey"), QStringLiteral("legacy-dialogue-key")},
        {QStringLiteral("model"), QString()},
        {QStringLiteral("daydream"), QJsonObject{
             {QStringLiteral("model"), QStringLiteral("legacy-daydream-model")},
             {QStringLiteral("maxTokens"), 700},
             {QStringLiteral("temperature"), 0.25}}}
    };
    const QJsonObject daydreamOnlyModelRoot{{QStringLiteral("aiSettings"), QJsonObject{
        {QStringLiteral("activeProfile"), QStringLiteral("default")},
        {QStringLiteral("profiles"), QJsonObject{
             {QStringLiteral("default"), daydreamOnlyModelProfile}}}}}};
    const QString secondPath = writeConfig(directory, daydreamOnlyModelRoot);
    QVERIFY(!secondPath.isEmpty());
    QVERIFY(ConfigManager::instance().loadConfig(secondPath));

    const ModelRoleConfig daydreamOnlyModel =
        ConfigManager::instance().getModelRoleConfig(ModelRole::Daydream);
    QCOMPARE(daydreamOnlyModel.routes.size(), 1);
    QCOMPARE(daydreamOnlyModel.routes.first().llm.model,
             QStringLiteral("legacy-daydream-model"));
}

void ModelRouterTests::configFor_whenRoleExists_shouldReturnOnlyRequestedRole() {
    ModelRoleRegistry registry({
        roleConfig(ModelRole::Dialogue,
                   {route(QStringLiteral("dialogue"), QStringLiteral("model-a"))}),
        roleConfig(ModelRole::Diary,
                   {route(QStringLiteral("diary"), QStringLiteral("model-b"))})
    });

    const auto result = registry.configFor(ModelRole::Diary);

    QVERIFY(result.isOk());
    QCOMPARE(result.value().role, ModelRole::Diary);
    QCOMPARE(result.value().routes.size(), 1);
    QCOMPARE(result.value().routes.first().routeId, QStringLiteral("diary"));
}

void ModelRouterTests::completeOnce_whenRouteIsValid_shouldDisableServiceLevelRetries() {
    auto transport = std::make_shared<CapturingLlmClient>();
    LlmChatService service(transport);
    LlmChatModelClient client(&service);
    ModelRouteConfig selected = route(
        QStringLiteral("dialogue-primary"), QStringLiteral("model-a"));
    selected.llm.retryCount = 5;
    bool callbackCalled = false;

    client.completeOnce(
        selected, {}, {},
        [&](bool success, LlmResponse response, QString error) {
            Q_UNUSED(error)
            callbackCalled = true;
            QVERIFY(success);
            QCOMPARE(response.content, QStringLiteral("ok"));
        },
        QStringLiteral("Milltina"));

    QVERIFY(callbackCalled);
    QCOMPARE(transport->configs.size(), 1);
    QCOMPARE(transport->configs.first().retryCount, 0);
}

QTEST_MAIN(ModelRouterTests)
#include "test_model_router.moc"
