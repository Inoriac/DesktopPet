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

struct CommandExecutionPolicy {
    QStringList allowedRoots;
    QStringList commandWhitelist;
    int timeoutMs = 5000;
    int maxOutputBytes = 32 * 1024;
};

// ================================================================
// 安全路径验证器
// 防止路径穿越攻击，确保操作限制在允许的根目录内
// ================================================================
class FilePathValidator {
public:
    explicit FilePathValidator(const QStringList& allowedRoots);

    bool isPathAllowed(const QString& path) const;
    bool isPathAllowedForWrite(const QString& path) const;
    QString normalizePath(const QString& path) const;
    QString normalizeFilePath(const QString& path) const;
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

// ================================================================
// Tool: write_text_file
// 功能: 在安全目录内写入文本文件
// 安全限制:
//   - 仅允许写入 allowedRoots 内路径
//   - 默认不覆盖已有文件，除非 overwrite=true
//   - 禁止写入常见敏感文件名/扩展名
//   - 限制单次写入大小
// ================================================================
class WriteTextFileTool : public AITool {
public:
    WriteTextFileTool(const QStringList& allowedRoots, int maxWriteBytes);

    QJsonObject parameterSchema() const override;
    ToolResult execute(const QJsonObject& params) override;

private:
    bool validate(const QJsonObject& params) const override;
    bool isSensitiveTarget(const QString& filePath) const;

private:
    FilePathValidator m_validator;
    int m_maxWriteBytes = 64 * 1024;
};

// ================================================================
// Tool: execute_whitelisted_command
// 功能: 在安全目录下执行白名单命令
// 安全限制:
//   - working_directory 必须位于 allowedRoots 内
//   - command 必须精确匹配 commandWhitelist
//   - 参数逐项传递给 QProcess，不拼接 shell 字符串
//   - 禁止 cmd.exe / powershell.exe / sh 等通用 shell
//   - 超时后强制终止
// ================================================================
class ExecuteWhitelistedCommandTool : public AITool {
public:
    explicit ExecuteWhitelistedCommandTool(const CommandExecutionPolicy& policy);

    QJsonObject parameterSchema() const override;
    ToolResult execute(const QJsonObject& params) override;

private:
    bool validate(const QJsonObject& params) const override;
    bool isCommandAllowed(const QString& command) const;
    bool isShellProgram(const QString& command) const;
    QStringList parseArguments(const QJsonObject& params) const;
    bool areArgumentsSafe(const QStringList& args,
                          const QString& workingDirectory,
                          QString& reason) const;
    bool isPathLikeArgument(const QString& arg, QString* candidatePath = nullptr) const;
    QString limitedOutput(const QByteArray& output) const;

private:
    CommandExecutionPolicy m_policy;
    FilePathValidator m_validator;
};

#endif // DESKTOP_PET_FILE_TOOLS_H