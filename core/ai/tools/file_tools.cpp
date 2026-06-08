//
// Created by Claude on 2026/6/8.
// 文件操作 Tools - 实现
//

#include "file_tools.h"
#include <QFile>
#include <QTextStream>
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QFileInfo>
#include <QDebug>

// ================================================================
// FilePathValidator 实现
// ================================================================

FilePathValidator::FilePathValidator(const QStringList& allowedRoots)
    : m_allowedRoots(allowedRoots) {
    // 预计算规范化的根路径
    for (const QString& root : allowedRoots) {
        QString normalized = QDir(root).absolutePath();
        // 确保路径以 / 结尾以便于前缀匹配
        if (!normalized.endsWith('/') && !normalized.endsWith('\\')) {
            normalized += '/';
        }
        m_normalizedRoots.append(normalized);
    }
}

bool FilePathValidator::isPathAllowed(const QString& path) const {
    const QString normalized = normalizePath(path);
    for (const QString& root : m_normalizedRoots) {
        if (normalized.startsWith(root, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

QString FilePathValidator::normalizePath(const QString& path) const {
    // 使用 QDir 获取规范化的绝对路径
    // 注意：对于不存在但可访问的路径（如临时目录），QDir 可以正确处理
    QDir dir(path);
    QString absolute = dir.absolutePath();
    // 统一使用正斜杠
    absolute.replace('\\', '/');
    // 确保末尾有分隔符
    if (!absolute.endsWith('/')) {
        absolute += '/';
    }
    return absolute;
}

QString FilePathValidator::getAllowedRoot(const QString& path) const {
    const QString normalized = normalizePath(path);
    for (int i = 0; i < m_normalizedRoots.size(); ++i) {
        if (normalized.startsWith(m_normalizedRoots[i], Qt::CaseInsensitive)) {
            return m_allowedRoots[i];
        }
    }
    return QString();
}

// ================================================================
// ReadTextFileTool 实现
// ================================================================

ReadTextFileTool::ReadTextFileTool(const QStringList& allowedRoots)
    : AITool(
        "read_text_file",
        "读取指定路径的文本文件内容。必须提供完整的文件路径。"
        "仅限访问应用目录和当前工作目录下的文件。",
        ToolCategory::Query
      )
    , m_validator(allowedRoots) {}

QJsonObject ReadTextFileTool::parameterSchema() const {
    QJsonObject schema;
    schema["type"] = "object";
    QJsonObject properties;
    QJsonObject path;
    path["type"] = "string";
    path["description"] = "要读取的文件完整路径";
    properties["path"] = path;
    QJsonObject max_lines;
    max_lines["type"] = "integer";
    max_lines["description"] = "最大读取行数，默认100";
    max_lines["default"] = DEFAULT_MAX_LINES;
    properties["max_lines"] = max_lines;
    QJsonObject encoding;
    encoding["type"] = "string";
    encoding["description"] = "文件编码，默认UTF-8，可选值：UTF-8, GBK, GB2312";
    encoding["default"] = "UTF-8";
    properties["encoding"] = encoding;
    schema["properties"] = properties;
    QJsonArray required;
    required.append("path");
    schema["required"] = required;
    return schema;
}

bool ReadTextFileTool::validate(const QJsonObject& params) const {
    // 检查必填字段
    if (!params.contains("path") || params.value("path").toString().isEmpty()) {
        return false;
    }
    return true;
}

ToolResult ReadTextFileTool::execute(const QJsonObject& params) {
    const QString filePath = params.value("path").toString();
    const int maxLines = params.value("max_lines").toInt(DEFAULT_MAX_LINES);
    const QString encoding = params.value("encoding").toString("UTF-8");

    // 路径安全检查
    if (!m_validator.isPathAllowed(filePath)) {
        return ToolResult::fail(QString("路径不允许访问: %1").arg(filePath));
    }

    // 检查文件是否存在
    QFileInfo fileInfo(filePath);
    if (!fileInfo.exists()) {
        return ToolResult::fail(QString("文件不存在: %1").arg(filePath));
    }

    // 检查是否为文件
    if (!fileInfo.isFile()) {
        return ToolResult::fail(QString("路径不是文件: %1").arg(filePath));
    }

    // 检查文件大小
    if (fileInfo.size() > MAX_FILE_SIZE) {
        return ToolResult::fail(QString("文件过大 (最大 %1 KB): %2")
            .arg(MAX_FILE_SIZE / 1024).arg(filePath));
    }

    // 读取文件
    const QString content = readWithEncoding(filePath, encoding, maxLines);

    QJsonObject result;
    result["path"] = filePath;
    result["size"] = fileInfo.size();
    result["lines"] = content.split('\n').size();
    result["content"] = content;
    result["truncated"] = (maxLines > 0 && content.split('\n').size() >= maxLines);

    return ToolResult::ok(result);
}

QString ReadTextFileTool::readWithEncoding(const QString& filePath, const QString& encoding, int maxLines) const {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }

    QTextStream stream(&file);
    if (encoding.toUpper() == "GBK" || encoding.toUpper() == "GB2312") {
        // Qt 在 Windows 上默认使用系统编码 (GBK)
        stream.setEncoding(QStringConverter::System);
    } else {
        // 默认 UTF-8
        stream.setEncoding(QStringConverter::Utf8);
    }

    QString result;
    int lineCount = 0;
    while (!stream.atEnd() && (maxLines <= 0 || lineCount < maxLines)) {
        result += stream.readLine() + '\n';
        lineCount++;
    }
    file.close();
    return result;
}

// ================================================================
// ListDirectoryTool 实现
// ================================================================

ListDirectoryTool::ListDirectoryTool(const QStringList& allowedRoots)
    : AITool(
        "list_directory",
        "列出指定目录下的所有文件和子目录。"
        "仅限访问应用目录和当前工作目录下的目录。",
        ToolCategory::Query
      )
    , m_validator(allowedRoots) {}

QJsonObject ListDirectoryTool::parameterSchema() const {
    QJsonObject schema;
    schema["type"] = "object";
    QJsonObject properties;
    QJsonObject path;
    path["type"] = "string";
    path["description"] = "要列出的目录完整路径";
    properties["path"] = path;
    QJsonObject include_hidden;
    include_hidden["type"] = "boolean";
    include_hidden["description"] = "是否包含隐藏文件/目录，默认false";
    include_hidden["default"] = false;
    properties["include_hidden"] = include_hidden;
    schema["properties"] = properties;
    QJsonArray required;
    required.append("path");
    schema["required"] = required;
    return schema;
}

bool ListDirectoryTool::validate(const QJsonObject& params) const {
    if (!params.contains("path") || params.value("path").toString().isEmpty()) {
        return false;
    }
    return true;
}

ToolResult ListDirectoryTool::execute(const QJsonObject& params) {
    const QString dirPath = params.value("path").toString();
    const bool includeHidden = params.value("include_hidden").toBool(false);

    // 路径安全检查
    if (!m_validator.isPathAllowed(dirPath)) {
        return ToolResult::fail(QString("路径不允许访问: %1").arg(dirPath));
    }

    // 检查目录是否存在
    QFileInfo dirInfo(dirPath);
    if (!dirInfo.exists()) {
        return ToolResult::fail(QString("目录不存在: %1").arg(dirPath));
    }

    // 检查是否为目录
    if (!dirInfo.isDir()) {
        return ToolResult::fail(QString("路径不是目录: %1").arg(dirPath));
    }

    QDir dir(dirPath);
    dir.setFilter(QDir::AllEntries | QDir::NoDotAndDotDot);

    // 获取条目列表
    const QFileInfoList entries = dir.entryInfoList();
    QJsonArray items;
    int count = 0;

    for (const QFileInfo& info : entries) {
        if (count >= MAX_ENTRIES) {
            break;
        }

        // 跳过隐藏文件（如果不需要）
        if (!includeHidden && info.fileName().startsWith('.')) {
            continue;
        }

        QJsonObject item;
        item["name"] = info.fileName();
        item["is_dir"] = info.isDir();
        item["size"] = info.size();
        item["modified"] = info.lastModified().toString("yyyy-MM-dd hh:mm:ss");
        item["extension"] = info.suffix();

        items.append(item);
        count++;
    }

    QJsonObject result;
    result["path"] = dirPath;
    result["count"] = items.size();
    result["entries"] = items;
    result["truncated"] = (count >= MAX_ENTRIES);

    return ToolResult::ok(result);
}