#pragma once
#include "../ai_tool.h"
#include "presentation/avatar_presentation.h"

class AvatarTool : public AITool {
  public:
    AvatarTool(QString name, QString description, ToolCategory category, AvatarPresentation* presentation)
        : AITool(std::move(name), std::move(description), category), m_presentation(presentation) {}
    QJsonObject parameterSchema() const override {
        return {{"type", "object"}, {"properties", QJsonObject{}}};
    }

  protected:
    ToolResult result(const QJsonObject& value) const {
        const auto status = value["status"].toString();
        if (QStringList{"rejected", "declined", "failed", "expired", "unknown"}.contains(status)) {
            auto r = ToolResult::fail(value["reason"].toString());
            r.data = value;
            return r;
        }
        return ToolResult::ok(value);
    }
    QPointer<AvatarPresentation> m_presentation;
};
class PlayAnimationTool : public AvatarTool {
  public:
    explicit PlayAnimationTool(AvatarPresentation* p)
        : AvatarTool(
              "play_animation",
              "申请当前形象的目标状态。先查询候选；状态名称及打断规则由形象配置。queued/"
              "started不代表完成，按request_id查询。能力不足可用（行为描述）表达，但不得宣称画面已执行。",
              ToolCategory::Action, p) {}
    QJsonObject parameterSchema() const override {
        QJsonArray names;
        if (m_presentation)
            for (const auto& s : m_presentation->capabilities()["states"].toArray())
                if (s.toObject()["requestable"].toBool(true))
                    names.append(s.toObject()["name"]);
        return {
            {"type", "object"},
            {"properties",
             QJsonObject{{"state", QJsonObject{{"type", "string"}, {"enum", names}}},
                         {"reason", QJsonObject{{"type", "string"}}},
                         {"ttl_ms", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 120000}}}}},
            {"required", QJsonArray{"state"}}};
    }
    ToolResult execute(const QJsonObject& params) override {
        if (!m_presentation)
            return ToolResult::fail("presentation_unavailable");
        return result(m_presentation->request(params["state"].toString(), AvatarRequestSource::Model,
                                              params["ttl_ms"].toInt(), params["reason"].toString()));
    }
};
class GetCurrentAnimationTool : public AvatarTool {
  public:
    explicit GetCurrentAnimationTool(AvatarPresentation* p)
        : AvatarTool("get_current_animation", "查询形象当前状态、播放、可见性与保护，不含底层渲染对象。",
                     ToolCategory::Query, p) {}
    ToolResult execute(const QJsonObject&) override {
        return m_presentation ? ToolResult::ok(m_presentation->snapshot())
                              : ToolResult::fail("presentation_unavailable");
    }
};
class GetIdleTransitionCandidatesTool : public AvatarTool {
  public:
    explicit GetIdleTransitionCandidatesTool(AvatarPresentation* p)
        : AvatarTool("get_idle_transition_candidates",
                     "查询当前形象可申请的状态和含义；不要求处于固定名称的待机状态。", ToolCategory::Query,
                     p) {}
    ToolResult execute(const QJsonObject&) override {
        return m_presentation ? ToolResult::ok(m_presentation->candidates())
                              : ToolResult::fail("presentation_unavailable");
    }
};
class GetActionTransitionStatusTool : public AvatarTool {
  public:
    explicit GetActionTransitionStatusTool(AvatarPresentation* p)
        : AvatarTool("get_action_transition_status",
                     "查询状态切换请求结果，或不传request_"
                     "id查询当前保护及队列。completed表示目标已实际启动，不等于表演轮次结束。",
                     ToolCategory::Query, p) {}
    QJsonObject parameterSchema() const override {
        return {{"type", "object"},
                {"properties", QJsonObject{{"request_id", QJsonObject{{"type", "string"}}}}}};
    }
    ToolResult execute(const QJsonObject& params) override {
        if (!m_presentation)
            return ToolResult::fail("presentation_unavailable");
        return result(params["request_id"].toString().isEmpty()
                          ? m_presentation->snapshot()
                          : m_presentation->requestResult(params["request_id"].toString()));
    }
};
class RequestIdleTransitionTool : public PlayAnimationTool {
  public:
    explicit RequestIdleTransitionTool(AvatarPresentation* p) : PlayAnimationTool(p) {
        m_name = "request_idle_transition";
    }
    QJsonObject parameterSchema() const override {
        auto schema = PlayAnimationTool::parameterSchema();
        auto properties = schema["properties"].toObject();
        properties["target_action"] = properties.take("state");
        schema["properties"] = properties;
        schema["required"] = QJsonArray{"target_action"};
        return schema;
    }
    ToolResult execute(const QJsonObject& params) override {
        auto normalized = params;
        if (!normalized.contains("state"))
            normalized["state"] = params.value("target_state").toString(params["target_action"].toString());
        return PlayAnimationTool::execute(normalized);
    }
};
class CancelAnimationRequestTool : public AvatarTool {
  public:
    explicit CancelAnimationRequestTool(AvatarPresentation* p)
        : AvatarTool("cancel_animation_request", "取消尚未完成的形象请求；受保护过渡到合法边界再收尾。",
                     ToolCategory::Action, p) {}
    QJsonObject parameterSchema() const override {
        return {{"type", "object"},
                {"properties", QJsonObject{{"request_id", QJsonObject{{"type", "string"}}}}},
                {"required", QJsonArray{"request_id"}}};
    }
    ToolResult execute(const QJsonObject& params) override {
        return m_presentation ? result(m_presentation->cancel(params["request_id"].toString()))
                              : ToolResult::fail("presentation_unavailable");
    }
};
class GetAvatarCapabilitiesTool : public AvatarTool {
  public:
    explicit GetAvatarCapabilitiesTool(AvatarPresentation* p)
        : AvatarTool("get_avatar_capabilities",
                     "获取当前形象独立外观描述、实际状态和能力；外观不改写人格或记忆。", ToolCategory::Query,
                     p) {}
    ToolResult execute(const QJsonObject&) override {
        return m_presentation ? ToolResult::ok(m_presentation->capabilities())
                              : ToolResult::fail("presentation_unavailable");
    }
};
class MoveAvatarTool : public AvatarTool {
  public:
    explicit MoveAvatarTool(AvatarPresentation* p)
        : AvatarTool("move_avatar", "以桌面Qt逻辑坐标移动形象窗口；与身体动画保护独立。",
                     ToolCategory::Action, p) {}
    QJsonObject parameterSchema() const override {
        return {{"type", "object"},
                {"properties", QJsonObject{{"x", QJsonObject{{"type", "integer"}}},
                                           {"y", QJsonObject{{"type", "integer"}}}}},
                {"required", QJsonArray{"x", "y"}}};
    }
    ToolResult execute(const QJsonObject& params) override {
        if (!m_presentation)
            return ToolResult::fail("presentation_unavailable");
        auto value = m_presentation->moveTo({params["x"].toInt(), params["y"].toInt()});
        return value["success"].toBool() ? ToolResult::ok(value)
                                         : ToolResult::fail(value["reason"].toString());
    }
};
