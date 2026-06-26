//
// Skill management tools — LLM-callable CRUD for autonomous skill learning
//

#include "skill_tools.h"
#include "skill_store.h"

#include <QJsonArray>
#include <QJsonDocument>

namespace {

constexpr int kMaxSteps = 20;
constexpr int kMaxTriggerPatterns = 15;
constexpr int kMaxTags = 20;
constexpr int kMaxNameLength = 80;
constexpr int kMaxDescriptionLength = 500;
constexpr int kMaxGoalLength = 300;
constexpr int kMaxInstructionLength = 400;
constexpr int kListLimit = 50;

QStringList jsonArrayToStringList(const QJsonArray& array, int maxItems = 0) {
    QStringList result;
    for (const QJsonValue& value : array) {
        const QString str = value.toString().trimmed();
        if (!str.isEmpty()) {
            result.append(str);
        }
        if (maxItems > 0 && result.size() >= maxItems) break;
    }
    return result;
}

QString clipped(const QString& text, int maxLength) {
    QString trimmed = text.trimmed();
    if (trimmed.size() > maxLength) {
        trimmed = trimmed.left(maxLength);
    }
    return trimmed;
}

QJsonObject makeStringProp(const QString& description) {
    QJsonObject obj;
    obj[QStringLiteral("type")] = QStringLiteral("string");
    obj[QStringLiteral("description")] = description;
    return obj;
}

QJsonObject makeStringArrayProp(const QString& description) {
    QJsonObject obj;
    obj[QStringLiteral("type")] = QStringLiteral("array");
    obj[QStringLiteral("description")] = description;
    QJsonObject items;
    items[QStringLiteral("type")] = QStringLiteral("string");
    obj[QStringLiteral("items")] = items;
    return obj;
}

QJsonObject makeStepsArrayProp(const QString& description) {
    QJsonObject obj;
    obj[QStringLiteral("type")] = QStringLiteral("array");
    obj[QStringLiteral("description")] = description;

    QJsonObject itemSchema;
    itemSchema[QStringLiteral("type")] = QStringLiteral("object");
    QJsonObject stepProps;
    stepProps[QStringLiteral("instruction")] = makeStringProp(QStringLiteral("步骤描述，应泛化而非特化"));
    stepProps[QStringLiteral("tool_hint")] = makeStringProp(QStringLiteral("建议使用的工具名称（可选）"));
    stepProps[QStringLiteral("condition")] = makeStringProp(QStringLiteral("执行此步骤的前置条件（可选）"));
    itemSchema[QStringLiteral("properties")] = stepProps;
    QJsonArray requiredFields;
    requiredFields.append(QStringLiteral("instruction"));
    itemSchema[QStringLiteral("required")] = requiredFields;

    obj[QStringLiteral("items")] = itemSchema;
    return obj;
}

QList<SkillStep> parseSteps(const QJsonArray& array) {
    QList<SkillStep> steps;
    for (int i = 0; i < array.size() && i < kMaxSteps; ++i) {
        const QJsonObject stepObj = array[i].toObject();
        SkillStep step;
        step.instruction = clipped(stepObj.value(QStringLiteral("instruction")).toString(), kMaxInstructionLength);
        step.toolHint = stepObj.value(QStringLiteral("tool_hint")).toString().trimmed();
        step.condition = stepObj.value(QStringLiteral("condition")).toString().trimmed();
        if (!step.instruction.isEmpty()) {
            steps.append(step);
        }
    }
    return steps;
}

QJsonObject skillSummary(const SkillEntry& entry) {
    QJsonObject obj;
    obj[QStringLiteral("id")] = entry.id;
    obj[QStringLiteral("name")] = entry.name;
    obj[QStringLiteral("domain")] = entry.domain;
    obj[QStringLiteral("description")] = entry.description;
    obj[QStringLiteral("use_count")] = entry.useCount;
    obj[QStringLiteral("success_rate")] = qRound(entry.successRate() * 100);
    obj[QStringLiteral("version")] = entry.version;
    obj[QStringLiteral("step_count")] = entry.steps.size();
    return obj;
}

}

// ── skill_create ──

SkillCreateTool::SkillCreateTool(SkillStore* store)
    : AITool(
          QStringLiteral("skill_create"),
          QStringLiteral(
              "将一个可复用的工作流固化为技能。当你发现某类任务的解决流程具有通用性，"
              "应提炼为泛化的技能以便日后自动匹配。技能的步骤描述应抽象化——"
              "使用参数占位而非硬编码具体值，使同一技能能应对同类不同实例的任务。"),
          ToolCategory::Action)
    , m_store(store) {}

QJsonObject SkillCreateTool::parameterSchema() const {
    QJsonObject schema;
    schema[QStringLiteral("type")] = QStringLiteral("object");

    QJsonObject properties;
    properties[QStringLiteral("name")] = makeStringProp(
        QStringLiteral("技能名称，简洁明确，如\"定时提醒设置\"\"文件内容搜索\""));
    properties[QStringLiteral("description")] = makeStringProp(
        QStringLiteral("技能描述，说明该技能能解决什么类型的问题（泛化描述，不要限定到具体场景）"));
    properties[QStringLiteral("domain")] = makeStringProp(
        QStringLiteral("技能所属领域，如 scheduling、file_management、music_control、web_search、companion"));
    properties[QStringLiteral("abstract_goal")] = makeStringProp(
        QStringLiteral("抽象目标描述，用于语义匹配。如\"帮助用户在指定时间收到提醒\""));
    properties[QStringLiteral("trigger_patterns")] = makeStringArrayProp(
        QStringLiteral("触发词/短语列表，用户输入匹配时激活此技能。应包含同义词和变体"));
    properties[QStringLiteral("tags")] = makeStringArrayProp(
        QStringLiteral("分类标签，用于检索和泛化匹配"));
    properties[QStringLiteral("steps")] = makeStepsArrayProp(
        QStringLiteral("有序步骤列表，每步描述应泛化，用{参数名}作占位符表示可变部分"));
    properties[QStringLiteral("required_tools")] = makeStringArrayProp(
        QStringLiteral("执行此技能所需的工具名列表"));
    properties[QStringLiteral("preconditions")] = makeStringArrayProp(
        QStringLiteral("执行前须满足的条件"));
    properties[QStringLiteral("postconditions")] = makeStringArrayProp(
        QStringLiteral("执行成功后的预期结果"));
    properties[QStringLiteral("parameter_schema")] = []() {
        QJsonObject obj;
        obj[QStringLiteral("type")] = QStringLiteral("object");
        obj[QStringLiteral("description")] = QStringLiteral(
            "技能参数的 JSON Schema，定义步骤中{参数名}占位符对应的参数。"
            "如 {\"time\": {\"type\": \"string\", \"description\": \"提醒时间\"}}");
        return obj;
    }();

    schema[QStringLiteral("properties")] = properties;

    QJsonArray required;
    required.append(QStringLiteral("name"));
    required.append(QStringLiteral("description"));
    required.append(QStringLiteral("steps"));
    schema[QStringLiteral("required")] = required;

    return schema;
}

ToolResult SkillCreateTool::execute(const QJsonObject& params) {
    if (!m_store) return ToolResult::fail(QStringLiteral("SkillStore 未配置"));

    const QString name = clipped(params.value(QStringLiteral("name")).toString(), kMaxNameLength);
    if (name.isEmpty()) return ToolResult::fail(QStringLiteral("name 不能为空"));

    if (m_store->findByName(name)) {
        return ToolResult::fail(QStringLiteral("已存在同名技能: %1").arg(name));
    }

    const QJsonArray stepsArray = params.value(QStringLiteral("steps")).toArray();
    if (stepsArray.isEmpty()) return ToolResult::fail(QStringLiteral("steps 不能为空"));

    SkillEntry entry;
    entry.name = name;
    entry.description = clipped(params.value(QStringLiteral("description")).toString(), kMaxDescriptionLength);
    entry.domain = params.value(QStringLiteral("domain")).toString().trimmed();
    entry.abstractGoal = clipped(params.value(QStringLiteral("abstract_goal")).toString(), kMaxGoalLength);
    entry.triggerPatterns = jsonArrayToStringList(
        params.value(QStringLiteral("trigger_patterns")).toArray(), kMaxTriggerPatterns);
    entry.tags = jsonArrayToStringList(params.value(QStringLiteral("tags")).toArray(), kMaxTags);
    entry.steps = parseSteps(stepsArray);
    entry.requiredTools = jsonArrayToStringList(params.value(QStringLiteral("required_tools")).toArray());
    entry.preconditions = jsonArrayToStringList(params.value(QStringLiteral("preconditions")).toArray());
    entry.postconditions = jsonArrayToStringList(params.value(QStringLiteral("postconditions")).toArray());
    entry.parameterSchema = params.value(QStringLiteral("parameter_schema")).toObject();

    if (entry.steps.isEmpty()) return ToolResult::fail(QStringLiteral("有效步骤为空"));

    const SkillEntry created = m_store->add(entry);

    QJsonObject result;
    result[QStringLiteral("created")] = true;
    result[QStringLiteral("id")] = created.id;
    result[QStringLiteral("name")] = created.name;
    result[QStringLiteral("step_count")] = created.steps.size();
    result[QStringLiteral("version")] = created.version;
    return ToolResult::ok(result);
}

// ── skill_update ──

SkillUpdateTool::SkillUpdateTool(SkillStore* store)
    : AITool(
          QStringLiteral("skill_update"),
          QStringLiteral("更新已有技能的步骤、触发词或描述。当技能执行后发现需要改进时调用。"),
          ToolCategory::Action)
    , m_store(store) {}

QJsonObject SkillUpdateTool::parameterSchema() const {
    QJsonObject schema;
    schema[QStringLiteral("type")] = QStringLiteral("object");

    QJsonObject properties;
    properties[QStringLiteral("id")] = makeStringProp(QStringLiteral("要更新的技能 ID"));
    properties[QStringLiteral("description")] = makeStringProp(QStringLiteral("新的描述（可选）"));
    properties[QStringLiteral("abstract_goal")] = makeStringProp(QStringLiteral("新的抽象目标（可选）"));
    properties[QStringLiteral("trigger_patterns")] = makeStringArrayProp(
        QStringLiteral("新的触发词列表（可选，会替换原有列表）"));
    properties[QStringLiteral("tags")] = makeStringArrayProp(QStringLiteral("新的标签列表（可选）"));
    properties[QStringLiteral("steps")] = makeStepsArrayProp(QStringLiteral("新的步骤列表（可选，会替换原有步骤）"));
    properties[QStringLiteral("preconditions")] = makeStringArrayProp(QStringLiteral("新的前置条件（可选）"));
    properties[QStringLiteral("postconditions")] = makeStringArrayProp(QStringLiteral("新的预期结果（可选）"));
    properties[QStringLiteral("parameter_schema")] = []() {
        QJsonObject obj;
        obj[QStringLiteral("type")] = QStringLiteral("object");
        obj[QStringLiteral("description")] = QStringLiteral("新的参数 Schema（可选）");
        return obj;
    }();

    schema[QStringLiteral("properties")] = properties;

    QJsonArray required;
    required.append(QStringLiteral("id"));
    schema[QStringLiteral("required")] = required;

    return schema;
}

ToolResult SkillUpdateTool::execute(const QJsonObject& params) {
    if (!m_store) return ToolResult::fail(QStringLiteral("SkillStore 未配置"));

    const QString id = params.value(QStringLiteral("id")).toString().trimmed();
    if (id.isEmpty()) return ToolResult::fail(QStringLiteral("id 不能为空"));

    const SkillEntry* existing = m_store->findById(id);
    if (!existing) return ToolResult::fail(QStringLiteral("未找到技能: %1").arg(id));

    SkillEntry updated = *existing;

    if (params.contains(QStringLiteral("description"))) {
        updated.description = clipped(params.value(QStringLiteral("description")).toString(), kMaxDescriptionLength);
    }
    if (params.contains(QStringLiteral("abstract_goal"))) {
        updated.abstractGoal = clipped(params.value(QStringLiteral("abstract_goal")).toString(), kMaxGoalLength);
    }
    if (params.contains(QStringLiteral("trigger_patterns"))) {
        updated.triggerPatterns = jsonArrayToStringList(
            params.value(QStringLiteral("trigger_patterns")).toArray(), kMaxTriggerPatterns);
    }
    if (params.contains(QStringLiteral("tags"))) {
        updated.tags = jsonArrayToStringList(params.value(QStringLiteral("tags")).toArray(), kMaxTags);
    }
    if (params.contains(QStringLiteral("steps"))) {
        const QList<SkillStep> newSteps = parseSteps(params.value(QStringLiteral("steps")).toArray());
        if (!newSteps.isEmpty()) {
            updated.steps = newSteps;
        }
    }
    if (params.contains(QStringLiteral("preconditions"))) {
        updated.preconditions = jsonArrayToStringList(params.value(QStringLiteral("preconditions")).toArray());
    }
    if (params.contains(QStringLiteral("postconditions"))) {
        updated.postconditions = jsonArrayToStringList(params.value(QStringLiteral("postconditions")).toArray());
    }
    if (params.contains(QStringLiteral("parameter_schema"))) {
        updated.parameterSchema = params.value(QStringLiteral("parameter_schema")).toObject();
    }

    if (!m_store->update(updated)) {
        return ToolResult::fail(QStringLiteral("更新失败"));
    }

    QJsonObject result;
    result[QStringLiteral("updated")] = true;
    result[QStringLiteral("id")] = updated.id;
    result[QStringLiteral("name")] = updated.name;
    result[QStringLiteral("version")] = updated.version;
    return ToolResult::ok(result);
}

// ── skill_list ──

SkillListTool::SkillListTool(SkillStore* store)
    : AITool(
          QStringLiteral("skill_list"),
          QStringLiteral("列出已保存的技能。可按领域或标签过滤。"),
          ToolCategory::Query)
    , m_store(store) {}

QJsonObject SkillListTool::parameterSchema() const {
    QJsonObject schema;
    schema[QStringLiteral("type")] = QStringLiteral("object");

    QJsonObject properties;
    properties[QStringLiteral("domain")] = makeStringProp(QStringLiteral("按领域筛选（可选）"));
    properties[QStringLiteral("tag")] = makeStringProp(QStringLiteral("按标签筛选（可选）"));

    schema[QStringLiteral("properties")] = properties;
    return schema;
}

ToolResult SkillListTool::execute(const QJsonObject& params) {
    if (!m_store) return ToolResult::fail(QStringLiteral("SkillStore 未配置"));

    const QString domain = params.value(QStringLiteral("domain")).toString().trimmed();
    const QString tag = params.value(QStringLiteral("tag")).toString().trimmed();

    QList<SkillEntry> entries;
    if (!domain.isEmpty()) {
        entries = m_store->findByDomain(domain);
    } else if (!tag.isEmpty()) {
        entries = m_store->findByTag(tag);
    } else {
        entries = m_store->all();
    }

    QJsonArray items;
    for (int i = 0; i < entries.size() && i < kListLimit; ++i) {
        items.append(skillSummary(entries[i]));
    }

    QJsonObject result;
    result[QStringLiteral("total")] = entries.size();
    result[QStringLiteral("items")] = items;
    return ToolResult::ok(result);
}

// ── skill_delete ──

SkillDeleteTool::SkillDeleteTool(SkillStore* store)
    : AITool(
          QStringLiteral("skill_delete"),
          QStringLiteral("删除一个已保存的技能。"),
          ToolCategory::Action)
    , m_store(store) {}

QJsonObject SkillDeleteTool::parameterSchema() const {
    QJsonObject schema;
    schema[QStringLiteral("type")] = QStringLiteral("object");

    QJsonObject properties;
    properties[QStringLiteral("id")] = makeStringProp(QStringLiteral("要删除的技能 ID"));

    schema[QStringLiteral("properties")] = properties;

    QJsonArray required;
    required.append(QStringLiteral("id"));
    schema[QStringLiteral("required")] = required;

    return schema;
}

ToolResult SkillDeleteTool::execute(const QJsonObject& params) {
    if (!m_store) return ToolResult::fail(QStringLiteral("SkillStore 未配置"));

    const QString id = params.value(QStringLiteral("id")).toString().trimmed();
    if (id.isEmpty()) return ToolResult::fail(QStringLiteral("id 不能为空"));

    const SkillEntry* existing = m_store->findById(id);
    if (!existing) return ToolResult::fail(QStringLiteral("未找到技能: %1").arg(id));

    const QString name = existing->name;
    if (!m_store->remove(id)) {
        return ToolResult::fail(QStringLiteral("删除失败"));
    }

    QJsonObject result;
    result[QStringLiteral("deleted")] = true;
    result[QStringLiteral("id")] = id;
    result[QStringLiteral("name")] = name;
    return ToolResult::ok(result);
}

// ── skill_record_outcome ──

SkillRecordOutcomeTool::SkillRecordOutcomeTool(SkillStore* store)
    : AITool(
          QStringLiteral("skill_record_outcome"),
          QStringLiteral("记录一次技能执行的结果（成功/失败），用于跟踪技能可靠性。"
                         "在按技能步骤完成任务后应调用此工具反馈结果。"),
          ToolCategory::Action)
    , m_store(store) {}

QJsonObject SkillRecordOutcomeTool::parameterSchema() const {
    QJsonObject schema;
    schema[QStringLiteral("type")] = QStringLiteral("object");

    QJsonObject properties;
    properties[QStringLiteral("id")] = makeStringProp(QStringLiteral("技能 ID"));

    QJsonObject successProp;
    successProp[QStringLiteral("type")] = QStringLiteral("integer");
    successProp[QStringLiteral("description")] = QStringLiteral("1=成功, 0=失败");
    properties[QStringLiteral("success")] = successProp;

    schema[QStringLiteral("properties")] = properties;

    QJsonArray required;
    required.append(QStringLiteral("id"));
    required.append(QStringLiteral("success"));
    schema[QStringLiteral("required")] = required;

    return schema;
}

ToolResult SkillRecordOutcomeTool::execute(const QJsonObject& params) {
    if (!m_store) return ToolResult::fail(QStringLiteral("SkillStore 未配置"));

    const QString id = params.value(QStringLiteral("id")).toString().trimmed();
    if (id.isEmpty()) return ToolResult::fail(QStringLiteral("id 不能为空"));

    const bool success = params.value(QStringLiteral("success")).toInt(0) != 0;

    if (!m_store->recordOutcome(id, success)) {
        return ToolResult::fail(QStringLiteral("未找到技能: %1").arg(id));
    }

    const SkillEntry* entry = m_store->findById(id);

    QJsonObject result;
    result[QStringLiteral("recorded")] = true;
    result[QStringLiteral("id")] = id;
    result[QStringLiteral("use_count")] = entry ? entry->useCount : 0;
    result[QStringLiteral("success_rate")] = entry ? qRound(entry->successRate() * 100) : 0;
    return ToolResult::ok(result);
}
