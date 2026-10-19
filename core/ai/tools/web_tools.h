#ifndef DESKTOP_PET_WEB_TOOLS_H
#define DESKTOP_PET_WEB_TOOLS_H

#include "../ai_tool.h"
#include "web_content_service.h"
#include <memory>

class WebFetchTool : public AITool {
public:
    explicit WebFetchTool(std::shared_ptr<WebContentService> service = {});
    QJsonObject parameterSchema() const override;
    ToolResult execute(const QJsonObject& params) override;

private:
    bool validate(const QJsonObject& params) const override;
    std::shared_ptr<WebContentService> m_service;
};

class WebSearchTool : public AITool {
public:
    explicit WebSearchTool(std::shared_ptr<WebContentService> service = {});
    QJsonObject parameterSchema() const override;
    ToolResult execute(const QJsonObject& params) override;

private:
    bool validate(const QJsonObject& params) const override;
    std::shared_ptr<WebContentService> m_service;
};

#endif
