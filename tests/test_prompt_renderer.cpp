//
// 提示词模版 + 独立身份基线测试
//

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QTest>
#include <QTemporaryDir>

#include "configLoader/config_manager.h"
#include "context_builder.h"
#include "identity/identity_baseline.h"
#include "identity/persona_projector.h"
#include "prompt/prompt_renderer.h"
#include "prompt/prompt_template_types.h"

namespace {

PromptTemplate makeDefaultTemplate() {
    PromptTemplate templ;
    templ.id = QStringLiteral("default");
    templ.name = QStringLiteral("默认通用模板");
    templ.systemPromptBody = QStringLiteral(
        "你是桌面宠物 {{pet_name}} 的AI大脑。"
        "{{persona_traits}}说话风格：{{speaking_style}}。"
        "目标：自然、简短、可执行。"
        "当可用工具能完成任务时，优先调用工具。"
        "回复尽量简洁，中文输出。"
        "你拥有技能学习能力：当完成了一个具有通用性的复杂任务流程后，"
        "可调用 skill_create 将其固化为可复用技能；"
        "在技能步骤中使用{参数名}占位符实现泛化。"
        "执行完技能后，调用 skill_record_outcome 反馈结果。");
    return templ;
}

} // namespace

class TestPromptRenderer : public QObject {
    Q_OBJECT

private slots:
    void testRenderSlotSubstitutionAndRedaction();
    void testRenderLeftoverRemoved();
    void testIdentityBaselineDefaults();
    void testIdentityBaselineParsingIsBounded();
    void testConfigManagerLoadsAndResetsIdentity();
    void testPersonaProjectionDoesNotExposeInternalState();
    void testPromptTemplateFromJson();
    void testContextBuilderFallbackUsesIdentity();
    void testContextBuilderRendersDefaultTemplate();
    void testCustomTemplateCannotRemoveSafetyRules();
};

void TestPromptRenderer::testRenderSlotSubstitutionAndRedaction() {
    QMap<QString, QString> vars;
    vars.insert(QStringLiteral("name"), QStringLiteral("小白"));
    vars.insert(QStringLiteral("style"), QStringLiteral("api_key=private"));
    const QString body = QStringLiteral("你好，我是{{name}}；{{style}}。");
    const QString rendered = PromptRenderer::render(body, vars);
    QCOMPARE(rendered, QStringLiteral("你好，我是小白；api_[redacted]=private。"));
}

void TestPromptRenderer::testRenderLeftoverRemoved() {
    QMap<QString, QString> vars;
    vars.insert(QStringLiteral("name"), QStringLiteral("小白"));
    const QString body = QStringLiteral("我是{{name}}，{{unknown_slot}}再见。{参数名}保留");
    QCOMPARE(PromptRenderer::render(body, vars), QStringLiteral("我是小白，再见。{参数名}保留"));
}

void TestPromptRenderer::testIdentityBaselineDefaults() {
    const IdentityBaseline baseline = IdentityBaseline::defaults();
    QCOMPARE(baseline.schemaVersion, 1);
    QCOMPARE(baseline.traits.value(QStringLiteral("sociability")), 0.45);
    QCOMPARE(baseline.traits.value(QStringLiteral("initiative")), 0.35);
    QCOMPARE(baseline.traits.value(QStringLiteral("openness")), 0.60);
    QCOMPARE(baseline.speakingStyle, QStringLiteral("自然、简短、重视真实经历"));
    QCOMPARE(baseline.anchorStrength, 0.95);
}

void TestPromptRenderer::testIdentityBaselineParsingIsBounded() {
    QJsonObject traits;
    traits.insert(QStringLiteral("sociability"), -2.0);
    traits.insert(QStringLiteral("initiative"), 3.0);
    traits.insert(QStringLiteral("invalid trait name"), 0.5);

    QJsonObject object;
    object.insert(QStringLiteral("schemaVersion"), 1);
    object.insert(QStringLiteral("traits"), traits);
    object.insert(QStringLiteral("speakingStyle"), QStringLiteral("  自然\n简短  "));
    object.insert(QStringLiteral("anchorStrength"), 9.0);

    const IdentityBaseline baseline = IdentityBaseline::fromJson(object);
    QCOMPARE(baseline.traits.value(QStringLiteral("sociability")), 0.0);
    QCOMPARE(baseline.traits.value(QStringLiteral("initiative")), 1.0);
    QVERIFY(!baseline.traits.contains(QStringLiteral("invalid trait name")));
    QCOMPARE(baseline.speakingStyle, QStringLiteral("自然 简短"));
    QCOMPARE(baseline.anchorStrength, 1.0);

    object.insert(QStringLiteral("schemaVersion"), 99);
    QCOMPARE(IdentityBaseline::fromJson(object).traits,
             IdentityBaseline::defaults().traits);
}

void TestPromptRenderer::testConfigManagerLoadsAndResetsIdentity() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    QJsonObject customTraits;
    customTraits.insert(QStringLiteral("initiative"), 0.8);
    QJsonObject customIdentity;
    customIdentity.insert(QStringLiteral("schemaVersion"), 1);
    customIdentity.insert(QStringLiteral("traits"), customTraits);
    customIdentity.insert(QStringLiteral("speakingStyle"), QStringLiteral("沉稳简洁"));
    customIdentity.insert(QStringLiteral("anchorStrength"), 0.7);
    QJsonObject customProfile;
    customProfile.insert(QStringLiteral("promptTemplate"), QStringLiteral("custom"));
    QJsonObject customProfiles;
    customProfiles.insert(QStringLiteral("default"), customProfile);
    QJsonObject customAi;
    customAi.insert(QStringLiteral("activeProfile"), QStringLiteral("default"));
    customAi.insert(QStringLiteral("identityBaseline"), customIdentity);
    customAi.insert(QStringLiteral("profiles"), customProfiles);
    QJsonObject customRoot;
    customRoot.insert(QStringLiteral("aiSettings"), customAi);

    const QString customPath = directory.filePath(QStringLiteral("custom.json"));
    QFile customFile(customPath);
    QVERIFY(customFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(customFile.write(QJsonDocument(customRoot).toJson(QJsonDocument::Compact)) > 0);
    customFile.close();

    ConfigManager& manager = ConfigManager::instance();
    QVERIFY(manager.loadConfig(customPath));
    QCOMPARE(manager.getIdentityBaseline().traits.value(QStringLiteral("initiative")), 0.8);
    QCOMPARE(manager.getIdentityBaseline().traits.value(QStringLiteral("openness")), 0.60);
    QCOMPARE(manager.getIdentityBaseline().speakingStyle, QStringLiteral("沉稳简洁"));
    QCOMPARE(manager.activePromptTemplateName(), QStringLiteral("custom"));

    QJsonObject defaultProfiles;
    defaultProfiles.insert(QStringLiteral("default"), QJsonObject{});
    QJsonObject defaultAi;
    defaultAi.insert(QStringLiteral("activeProfile"), QStringLiteral("default"));
    defaultAi.insert(QStringLiteral("profiles"), defaultProfiles);
    QJsonObject defaultRoot;
    defaultRoot.insert(QStringLiteral("aiSettings"), defaultAi);

    const QString defaultPath = directory.filePath(QStringLiteral("default.json"));
    QFile defaultFile(defaultPath);
    QVERIFY(defaultFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(defaultFile.write(QJsonDocument(defaultRoot).toJson(QJsonDocument::Compact)) > 0);
    defaultFile.close();

    QVERIFY(manager.loadConfig(defaultPath));
    QCOMPARE(manager.getIdentityBaseline().traits, IdentityBaseline::defaults().traits);
    QCOMPARE(manager.activePromptTemplateName(), QStringLiteral("default"));
}

void TestPromptRenderer::testPersonaProjectionDoesNotExposeInternalState() {
    IdentityBaseline baseline = IdentityBaseline::defaults();
    baseline.traits.insert(QStringLiteral("private_evidence"), 0.99);
    baseline.anchorStrength = 0.73;

    const PersonaProjection projection = PersonaProjector().projectBaseline(
        baseline, QStringLiteral("Milltina"));
    QCOMPARE(projection.promptSlots.keys(),
             QStringList({QStringLiteral("persona_traits"),
                          QStringLiteral("pet_name"),
                          QStringLiteral("speaking_style")}));

    const QString joined = projection.promptSlots.values().join(QStringLiteral(" "));
    QVERIFY(joined.contains(QStringLiteral("适时主动")));
    QVERIFY(joined.contains(QStringLiteral("稳健开放")));
    QVERIFY(joined.contains(QStringLiteral("交流适度")));
    QVERIFY(!joined.contains(QStringLiteral("private_evidence")));
    QVERIFY(!joined.contains(QStringLiteral("0.73")));
    QVERIFY(!joined.contains(QStringLiteral("0.99")));
    QVERIFY(!joined.contains(QStringLiteral("anchorStrength")));
}

void TestPromptRenderer::testPromptTemplateFromJson() {
    QJsonObject object;
    object.insert(QStringLiteral("id"), QStringLiteral("default"));
    object.insert(QStringLiteral("name"), QStringLiteral("默认通用模板"));
    object.insert(QStringLiteral("systemPromptBody"), QStringLiteral("hello {{pet_name}}"));
    object.insert(QStringLiteral("slots"), QJsonArray{QStringLiteral("pet_name")});
    object.insert(QStringLiteral("version"), 2);

    const PromptTemplate templ = PromptTemplate::fromJson(object);
    QCOMPARE(templ.id, QStringLiteral("default"));
    QCOMPARE(templ.systemPromptBody, QStringLiteral("hello {{pet_name}}"));
    QCOMPARE(templ.slotNames, QStringList{QStringLiteral("pet_name")});
    QCOMPARE(templ.version, 2);
}

void TestPromptRenderer::testContextBuilderFallbackUsesIdentity() {
    ContextBuilder builder;
    const QString prompt = builder.buildSystemPrompt(QStringLiteral("Milltina"));
    QVERIFY(prompt.contains(QStringLiteral("你是桌面宠物 Milltina 的AI大脑。")));
    QVERIFY(prompt.contains(QStringLiteral("性格倾向：适时主动、稳健开放、交流适度。")));
    QVERIFY(prompt.contains(QStringLiteral("说话风格：自然、简短、重视真实经历。")));
    QVERIFY(prompt.contains(QStringLiteral("调用 skill_record_outcome 反馈结果。")));
    QVERIFY(prompt.contains(QStringLiteral("不得降低命令成功率")));
    QVERIFY(!prompt.contains(QStringLiteral("{{")));
}

void TestPromptRenderer::testContextBuilderRendersDefaultTemplate() {
    IdentityBaseline baseline = IdentityBaseline::defaults();
    baseline.traits.insert(QStringLiteral("initiative"), 0.9);
    baseline.speakingStyle = QStringLiteral("口语化、短句为主、不夸张");

    ContextBuilder builder;
    builder.setPromptTemplate(makeDefaultTemplate());
    builder.setIdentityBaseline(baseline);

    const QString prompt = builder.buildSystemPrompt(QStringLiteral("Milltina"));
    QVERIFY(prompt.contains(QStringLiteral("较主动")));
    QVERIFY(prompt.contains(QStringLiteral("口语化、短句为主、不夸张")));
    QVERIFY(prompt.contains(QStringLiteral("当可用工具能完成任务时，优先调用工具。")));
    QVERIFY(prompt.contains(QStringLiteral("在技能步骤中使用{参数名}占位符实现泛化。")));
    QVERIFY(prompt.contains(QStringLiteral("不得把桌宠的情绪状态描述成对用户心理状态的判断或诊断。")));
}

void TestPromptRenderer::testCustomTemplateCannotRemoveSafetyRules() {
    PromptTemplate custom;
    custom.id = QStringLiteral("custom");
    custom.systemPromptBody = QStringLiteral("只保留自定义正文：{{pet_name}}");

    ContextBuilder builder;
    builder.setPromptTemplate(custom);
    const QString prompt = builder.buildSystemPrompt(QStringLiteral("小白"));
    QVERIFY(prompt.startsWith(QStringLiteral("只保留自定义正文：小白")));
    QVERIFY(prompt.contains(QStringLiteral("不得降低命令成功率")));
    QVERIFY(prompt.contains(QStringLiteral("不得用悲伤、生气、内疚")));
    QVERIFY(prompt.contains(QStringLiteral("不得把桌宠的情绪状态描述成对用户心理状态的判断或诊断")));
}

QTEST_MAIN(TestPromptRenderer)
#include "test_prompt_renderer.moc"
