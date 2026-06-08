//
// Created by Claude on 2026/6/8.
// 网络工具
// 提供网页获取和搜索功能，带有安全防护
//

#ifndef DESKTOP_PET_WEB_TOOLS_H
#define DESKTOP_PET_WEB_TOOLS_H

#include "../ai_tool.h"
#include <QString>
#include <QHostAddress>

// ================================================================
// 网络安全验证器
// 防止 SSRF 攻击，拦截私有/保留 IP 段
// ================================================================
class NetworkSecurityValidator {
public:
    NetworkSecurityValidator();

    bool isUrlAllowed(const QString& url) const;
    bool isIpBlocked(const QString& host) const;
    static QString resolveHost(const QString& host);

private:
    bool isPrivateIp(const QHostAddress& addr) const;
    bool isReservedIp(const QHostAddress& addr) const;
};

// ================================================================
// Tool: web_fetch
// 功能: 获取网页内容
// 参数:
//   - url: 目标 URL (必填)
//   - timeout_ms: 超时时间毫秒 (可选, 默认15000)
// ================================================================
class WebFetchTool : public AITool {
public:
    WebFetchTool();

    QJsonObject parameterSchema() const override;
    ToolResult execute(const QJsonObject& params) override;

private:
    bool validate(const QJsonObject& params) const override;

private:
    NetworkSecurityValidator m_validator;
    static constexpr int DEFAULT_TIMEOUT_MS = 15000;
    static constexpr qint64 MAX_RESPONSE_SIZE = 500 * 1024;  // 500KB
};

// ================================================================
// Tool: web_search
// 功能: 使用 Bing 搜索
// 参数:
//   - query: 搜索关键词 (必填)
//   - max_results: 最大结果数 (可选, 默认5)
// ================================================================
class WebSearchTool : public AITool {
public:
    WebSearchTool();

    QJsonObject parameterSchema() const override;
    ToolResult execute(const QJsonObject& params) override;

private:
    bool validate(const QJsonObject& params) const override;
    QString bingSearch(const QString& query, int maxResults) const;
    QJsonArray parseBingResults(const QString& html) const;
    QString cleanHtml(const QString& html) const;

private:
    NetworkSecurityValidator m_validator;
    static constexpr int DEFAULT_MAX_RESULTS = 5;
    static constexpr int BING_TIMEOUT_MS = 15000;
};

#endif // DESKTOP_PET_WEB_TOOLS_H