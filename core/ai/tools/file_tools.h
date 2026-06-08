//
// Created by Claude on 2026/6/8.
// 文件操作 Tools
// 提供文件读取和目录列表功能，带有安全路径限制
//

#ifndef DESKTOP_PET_FILE_TOOLS_H
#define DESKTOP_PET_FILE_TOOLS_H

#include "../ai_tool.h"
#include <QStringList>
#include <QDir>

// ================================================================
// 安全路径验证器
// 防止路径穿越攻击，确保操作限制在允许的根目录内
// ================================================================
class FilePathValidator {
public:
    explicit FilePathValidator(const QStringList& allowedRoots);

    bool isPathAllowed(const QString& path) const;
    QString normalizePath(const QString& path) const;
    QString getAllowedRoot(const QString& path) const;

private:
    QStringList m_allowedRoots;
    QStringList m_normalizedRoots;
};

// ================================================================
// Tool: read_text_file
// 功能: 读取文本文件内容
// 参数:
//   - path: 文件路径 (必填)
//   - max_lines: 最大行数 (可选, 默认100)
//   - encoding: 编码类型 (可选, 默认UTF-8)
// ================================================================
class ReadTextFileTool : public AITool {
public:
    explicit ReadTextFileTool(const QStringList& allowedRoots);

    QJsonObject parameterSchema() const override;
    ToolResult execute(const QJsonObject& params) override;

private:
    bool validate(const QJsonObject& params) const override;
    QString detectEncoding(const QString& filePath) const;
    QString readWithEncoding(const QString& filePath, const QString& encoding, int maxLines) const;

private:
    FilePathValidator m_validator;
    static constexpr qint64 MAX_FILE_SIZE = 100 * 1024;  // 100KB
    static constexpr int DEFAULT_MAX_LINES = 100;
};

// ================================================================
// Tool: list_directory
// 功能: 列出目录内容
// 参数:
//   - path: 目录路径 (必填)
//   - include_hidden: 是否包含隐藏文件 (可选, 默认false)
// ================================================================
class ListDirectoryTool : public AITool {
public:
    explicit ListDirectoryTool(const QStringList& allowedRoots);

    QJsonObject parameterSchema() const override;
    ToolResult execute(const QJsonObject& params) override;

private:
    bool validate(const QJsonObject& params) const override;

private:
    FilePathValidator m_validator;
    static constexpr int MAX_ENTRIES = 200;
};

#endif // DESKTOP_PET_FILE_TOOLS_H