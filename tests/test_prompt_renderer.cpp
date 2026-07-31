//
// 提示词模版 + 性格预设 渲染测试
// 覆盖：PromptRenderer 的 slot 替换/残留清理/拼句/脱敏，
//       PetPersonality 与 PromptTemplate 的 fromJson，
//       ContextBuilder 的零回归兜底与渲染化行为。
//

#include <QJsonArray>
#include <QJsonObject>
#include <QTest>

#include "context_builder.h"
#include "pet_personality.h"
#include "prompt/prompt_renderer.h"
#include "prompt/prompt_template_types.h"

// 与 config/prompts/default.json 的 systemPromptBody 保持一致；用于 ContextBuilder 渲染测试。
static PromptTemplate makeDefaultTemplate() {
    PromptTemplate templ;
    templ.id = QStringLiteral("default");
    templ.name = QStringLiteral("默认通用模板");
    templ.systemPromptBody = QStringLiteral(
        "你是桌面宠物 {{pet_name}} 的AI大脑。"
        "{{persona_traits}}语气与说话风格：{{speaking_style}}。"
        "{{catchphrases}}"
        "目标：自然、简短、可执行。"
        "当可用工具能完成任务时，优先调用工具。"
        "回复尽量简洁，中文输出。"
        "你拥有技能学习能力：当完成了一个具有通用性的复杂任务流程后，"
        "可调用 skill_create 将其固化为可复用技能；"
        "在技能步骤中使用{参数名}占位符实现泛化。"
        "执行完技能后，调用 skill_record_outcome 反馈结果。"
        "{{extra_directives}}");
    return templ;
}

class TestPromptRenderer : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void testRenderSlotSubstitution();
    void testRenderLeftoverRemoved();
    void testBuildVariablesBaseline();
    void testBuildVariablesEmptyPersona();
    void testRedactSecrets();
    void testPetPersonalityFromJson();
    void testPromptTemplateFromJson();
    void testContextBuilderFallbackZeroRegression();
    void testContextBuilderRendersDefaultTemplate();
    void testContextBuilderSwitchesPersona();
};

void TestPromptRenderer::initTestCase() {
    qDebug() << "Starting PromptRenderer unit tests...";
}

void TestPromptRenderer::testRenderSlotSubstitution() {
    QMap<QString, QString> vars;
    vars[QStringLiteral("name")] = QStringLiteral("小白");
    vars[QStringLiteral("mood")] = QStringLiteral("开心");
    const QString body = QStringLiteral("你好，我是{{name}}，今天{{mood}}。");
    QCOMPARE(PromptRenderer::render(body, vars), QStringLiteral("你好，我是小白，今天开心。"));
}

void TestPromptRenderer::testRenderLeftoverRemoved() {
    // 提供的 slot 被替换；未提供的 {{unknown_slot}} 收敛为空，不影响单括号 {参数名}。
    QMap<QString, QString> vars;
    vars[QStringLiteral("name")] = QStringLiteral("小白");
    const QString body = QStringLiteral("我是{{name}}，{{unknown_slot}}再见。{参数名}保留");
    QCOMPARE(PromptRenderer::render(body, vars), QStringLiteral("我是小白，再见。{参数名}保留"));
}

void TestPromptRenderer::testBuildVariablesBaseline() {
    PetPersonality persona;
    persona.gender = QStringLiteral("female");
    persona.traits = {QStringLiteral("平和"), QStringLiteral("体贴"), QStringLiteral("话不多")};
    persona.speakingStyle = QStringLiteral("口语化、短句为主、不夸张");
    persona.catchphrases = {QStringLiteral("诶嘿"), QStringLiteral("哎呀我又忘了")};
    persona.extraDirectives = {QStringLiteral("第一条"), QStringLiteral("第二条")};

    const QMap<QString, QString> vars = PromptRenderer::buildVariables(persona, QStringLiteral("Milltina"));
    QCOMPARE(vars.value(QStringLiteral("pet_name")), QStringLiteral("Milltina"));
    QCOMPARE(vars.value(QStringLiteral("gender")), QStringLiteral("female"));
    QCOMPARE(vars.value(QStringLiteral("persona_traits")), QStringLiteral("性格：平和、体贴、话不多。"));
    QCOMPARE(vars.value(QStringLiteral("speaking_style")), QStringLiteral("口语化、短句为主、不夸张"));
    QCOMPARE(vars.value(QStringLiteral("catchphrases")), QStringLiteral("口头禅：诶嘿、哎呀我又忘了。"));
    QVERIFY(vars.value(QStringLiteral("extra_directives")).contains(QStringLiteral("额外要求：")));
    QVERIFY(vars.value(QStringLiteral("extra_directives")).contains(QStringLiteral("第一条")));
    QVERIFY(vars.value(QStringLiteral("extra_directives")).contains(QStringLiteral("第二条")));
}

void TestPromptRenderer::testBuildVariablesEmptyPersona() {
    // 默认构造的 Persona：列表字段为空 → 对应变量为空，pet_name 缺失回退「桌宠」。
    const PetPersonality persona;
    const QMap<QString, QString> vars = PromptRenderer::buildVariables(persona, QString());
    QCOMPARE(vars.value(QStringLiteral("pet_name")), QStringLiteral("桌宠"));
    QCOMPARE(vars.value(QStringLiteral("gender")), QStringLiteral("neutral"));
    QVERIFY(vars.value(QStringLiteral("persona_traits")).isEmpty());
    QVERIFY(vars.value(QStringLiteral("catchphrases")).isEmpty());
    QVERIFY(vars.value(QStringLiteral("extra_directives")).isEmpty());
}

void TestPromptRenderer::testRedactSecrets() {
    QVERIFY(PromptRenderer::redactSecrets(QStringLiteral("my api_key is 123")).contains(QStringLiteral("api_[redacted]")));
    QVERIFY(PromptRenderer::redactSecrets(QStringLiteral("Password: abc")).contains(QStringLiteral("pass[redacted]")));
    QVERIFY(PromptRenderer::redactSecrets(QStringLiteral("some_token=zzz")).contains(QStringLiteral("tok[redacted]")));
    QVERIFY(PromptRenderer::redactSecrets(QStringLiteral("a secret value")).contains(QStringLiteral("sec[redacted]")));
    QCOMPARE(PromptRenderer::redactSecrets(QStringLiteral("nothing here")), QStringLiteral("nothing here"));
}

void TestPromptRenderer::testPetPersonalityFromJson() {
    QJsonObject obj;
    obj[QStringLiteral("name")] = QStringLiteral("健忘型");
    obj[QStringLiteral("forgetProbability")] = 0.35;
    obj[QStringLiteral("randomVariance")] = 20;
    obj[QStringLiteral("gender")] = QStringLiteral("female");
    obj[QStringLiteral("tone")] = QStringLiteral("迷糊、可爱、偶尔忘事");
    obj[QStringLiteral("traits")] = QJsonArray{QStringLiteral("迷糊"), QStringLiteral("热心"), QStringLiteral("爱唠叨")};
    obj[QStringLiteral("speakingStyle")] = QStringLiteral("语气轻快、多用「～」、偶尔自我打岔");
    obj[QStringLiteral("catchphrases")] = QJsonArray{QStringLiteral("诶嘿"), QStringLiteral("哎呀我又忘了")};
    obj[QStringLiteral("extraDirectives")] = QJsonArray{QStringLiteral("若用户提到待办但未明确时间，可主动追问大致时间以降低遗忘影响。")};

    const PetPersonality p = PetPersonality::fromJson(obj);
    QCOMPARE(p.name, QStringLiteral("健忘型"));
    QCOMPARE(p.gender, QStringLiteral("female"));
    QCOMPARE(p.tone, QStringLiteral("迷糊、可爱、偶尔忘事"));
    QVERIFY((p.traits == QStringList{QStringLiteral("迷糊"), QStringLiteral("热心"), QStringLiteral("爱唠叨")}));
    QVERIFY((p.catchphrases == QStringList{QStringLiteral("诶嘿"), QStringLiteral("哎呀我又忘了")}));
    QVERIFY(p.extraDirectives.size() == 1);
    QVERIFY(p.forgetProbability > 0.3 && p.forgetProbability < 0.4);
    QCOMPARE(p.randomVariance, 20);
}

void TestPromptRenderer::testPromptTemplateFromJson() {
    QJsonObject obj;
    obj[QStringLiteral("id")] = QStringLiteral("default");
    obj[QStringLiteral("name")] = QStringLiteral("默认通用模板");
    obj[QStringLiteral("systemPromptBody")] = QStringLiteral("hello {{pet_name}}");
    obj[QStringLiteral("slots")] = QJsonArray{QStringLiteral("pet_name"), QStringLiteral("tone")};
    obj[QStringLiteral("version")] = 2;

    const PromptTemplate t = PromptTemplate::fromJson(obj);
    QCOMPARE(t.id, QStringLiteral("default"));
    QCOMPARE(t.name, QStringLiteral("默认通用模板"));
    QCOMPARE(t.systemPromptBody, QStringLiteral("hello {{pet_name}}"));
    QVERIFY((t.slotNames == QStringList{QStringLiteral("pet_name"), QStringLiteral("tone")}));
    QCOMPARE(t.version, 2);
}

void TestPromptRenderer::testContextBuilderFallbackZeroRegression() {
    // 未注入模版时回退内联兜底，与改造前硬编码系统提示词逐字一致 → 零回归。
    ContextBuilder cb;
    const QString expected = QStringLiteral(
        "你是桌面宠物 Milltina 的AI大脑。"
        "目标：自然、简短、可执行。"
        "当可用工具能完成任务时，优先调用工具。"
        "回复尽量简洁，中文输出。"
        "你拥有技能学习能力：当完成了一个具有通用性的复杂任务流程后，"
        "可调用 skill_create 将其固化为可复用技能；"
        "在技能步骤中使用{参数名}占位符实现泛化。"
        "执行完技能后，调用 skill_record_outcome 反馈结果。");
    QCOMPARE(cb.buildSystemPrompt(QStringLiteral("Milltina")), expected);
    // 空 petName 回退「桌宠」。
    QVERIFY(cb.buildSystemPrompt(QString()).startsWith(QStringLiteral("你是桌面宠物 桌宠 的AI大脑。")));
}

void TestPromptRenderer::testContextBuilderRendersDefaultTemplate() {
    PetPersonality persona;  // 温和型基线
    persona.traits = {QStringLiteral("平和"), QStringLiteral("体贴"), QStringLiteral("话不多")};
    persona.speakingStyle = QStringLiteral("口语化、短句为主、不夸张");

    ContextBuilder cb;
    cb.setPromptTemplate(makeDefaultTemplate());
    cb.setPersona(persona);

    const QString out = cb.buildSystemPrompt(QStringLiteral("Milltina"));
    QVERIFY(out.contains(QStringLiteral("你是桌面宠物 Milltina 的AI大脑。")));
    QVERIFY(out.contains(QStringLiteral("性格：平和、体贴、话不多。")));
    QVERIFY(out.contains(QStringLiteral("语气与说话风格：口语化、短句为主、不夸张。")));
    // 能力规则段逐句保留（零回归）。
    QVERIFY(out.contains(QStringLiteral("目标：自然、简短、可执行。")));
    QVERIFY(out.contains(QStringLiteral("当可用工具能完成任务时，优先调用工具。")));
    QVERIFY(out.contains(QStringLiteral("回复尽量简洁，中文输出。")));
    QVERIFY(out.contains(QStringLiteral("在技能步骤中使用{参数名}占位符实现泛化。")));
    QVERIFY(out.contains(QStringLiteral("调用 skill_record_outcome 反馈结果。")));
    // 无残留占位、温和型无口头禅。
    QVERIFY(!out.contains(QStringLiteral("{{")));
    QVERIFY(!out.contains(QStringLiteral("口头禅")));
}

void TestPromptRenderer::testContextBuilderSwitchesPersona() {
    PetPersonality forgetful;
    forgetful.name = QStringLiteral("健忘型");
    forgetful.traits = {QStringLiteral("迷糊"), QStringLiteral("热心"), QStringLiteral("爱唠叨")};
    forgetful.speakingStyle = QStringLiteral("语气轻快");
    forgetful.catchphrases = {QStringLiteral("诶嘿"), QStringLiteral("哎呀我又忘了")};
    forgetful.extraDirectives = {QStringLiteral("若用户提到待办但未明确时间，可主动追问大致时间以降低遗忘影响。")};

    ContextBuilder cb;
    cb.setPromptTemplate(makeDefaultTemplate());
    cb.setPersona(forgetful);

    const QString out = cb.buildSystemPrompt(QStringLiteral("小白"));
    QVERIFY(out.contains(QStringLiteral("你是桌面宠物 小白 的AI大脑。")));
    QVERIFY(out.contains(QStringLiteral("口头禅：诶嘿、哎呀我又忘了。")));
    QVERIFY(out.contains(QStringLiteral("额外要求：")));
    QVERIFY(out.contains(QStringLiteral("若用户提到待办但未明确时间")));
    QVERIFY(!out.contains(QStringLiteral("{{")));
}

QTEST_MAIN(TestPromptRenderer)
#include "test_prompt_renderer.moc"
