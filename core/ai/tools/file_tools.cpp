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
#include <QProcess>
#include <QSaveFile>
#include <QRegularExpression>

namespace {
Qt::CaseSensitivity pathCaseSensitivity() {
#ifdef Q_OS_WIN
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

QString cleanAbsolutePath(const QString& path) {
    QString result = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    result.replace('\\', '/');
    return result;
}
}

// ================================================================
// FilePathValidator 实现
// ================================================================

FilePathValidator::FilePathValidator(const QStringList& allowedRoots) {
    // 预计算规范化的根路径
    for (const QString& root : allowedRoots) {
        QString normalized = QFileInfo(root).canonicalFilePath();
        if (normalized.isEmpty()) {
            normalized = QDir(root).canonicalPath();
        }
        if (normalized.isEmpty()) {
            continue;
        }
        normalized = QDir::cleanPath(normalized);
        normalized.replace('\\', '/');
        m_allowedRoots.append(root);
        m_normalizedRoots.append(normalized);
    }
}

bool FilePathValidator::isPathAllowed(const QString& path) const {
    return isWithinAllowedRoot(resolvePathForValidation(path));
}

bool FilePathValidator::isPathAllowedForWrite(const QString& path) const {
    return isWithinAllowedRoot(resolvePathForValidation(path));
}

bool FilePathValidator::isWithinAllowedRoot(const QString& resolvedPath) const {
    if (resolvedPath.isEmpty()) {
        return false;
    }
    for (const QString& root : m_normalizedRoots) {
        const QString rootPrefix = root.endsWith('/') ? root : root + '/';
        if (resolvedPath.compare(root, pathCaseSensitivity()) == 0
            || resolvedPath.startsWith(rootPrefix, pathCaseSensitivity())) {
            return true;
        }
    }
    return false;
}

QString FilePathValidator::normalizePath(const QString& path) const {
    const QString resolved = resolvePathForValidation(path);
    return resolved.isEmpty() ? cleanAbsolutePath(path) : resolved;
}

QString FilePathValidator::normalizeFilePath(const QString& path) const {
    return cleanAbsolutePath(path);
}

QString FilePathValidator::resolvePathForValidation(const QString& path) const {
    const QString absolute = cleanAbsolutePath(path);
    QFileInfo current(absolute);
    QStringList missingComponents;

    while (!current.exists() && !current.isSymLink()) {
        const QString name = current.fileName();
        if (name.isEmpty()) {
            return {};
        }
        missingComponents.prepend(name);
        const QString parentPath = current.absolutePath();
        if (parentPath == current.absoluteFilePath()) {
            return {};
        }
        current.setFile(parentPath);
    }

    QString resolved = current.canonicalFilePath();
    if (resolved.isEmpty() && current.isDir()) {
        resolved = QDir(current.absoluteFilePath()).canonicalPath();
    }
    if (resolved.isEmpty()) {
        return {};
    }

    resolved = QDir::cleanPath(resolved);
    for (const QString& component : missingComponents) {
        resolved = QDir(resolved).filePath(component);
    }
    resolved = QDir::cleanPath(resolved);
    resolved.replace('\\', '/');
    return resolved;
}

QString FilePathValidator::getAllowedRoot(const QString& path) const {
    const QString normalized = resolvePathForValidation(path);
    for (int i = 0; i < m_normalizedRoots.size(); ++i) {
        const QString rootPrefix = m_normalizedRoots[i].endsWith('/')
            ? m_normalizedRoots[i]
            : m_normalizedRoots[i] + '/';
        if (normalized.compare(m_normalizedRoots[i], pathCaseSensitivity()) == 0
            || normalized.startsWith(rootPrefix, pathCaseSensitivity())) {
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
    const QString filePath = m_validator.normalizeFilePath(params.value("path").toString());
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
    const QString dirPath = m_validator.normalizeFilePath(params.value("path").toString());
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

// ================================================================
// WriteTextFileTool 实现
// ================================================================

WriteTextFileTool::WriteTextFileTool(const QStringList& allowedRoots, int maxWriteBytes)
    : AITool(
        "write_text_file",
        "在用户授权的安全目录内写入文本文件。默认不覆盖已有文件；禁止写入密钥、证书、可执行脚本等敏感目标。",
        ToolCategory::Action
      )
    , m_validator(allowedRoots)
    , m_maxWriteBytes(qBound(1, maxWriteBytes, 1024 * 1024)) {}

QJsonObject WriteTextFileTool::parameterSchema() const {
    QJsonObject schema;
    schema["type"] = "object";

    QJsonObject properties;
    properties["path"] = QJsonObject{
        {"type", "string"},
        {"description", "要写入的目标文件路径，必须位于安全目录内"}
    };
    properties["content"] = QJsonObject{
        {"type", "string"},
        {"description", "要写入的文本内容"}
    };
    properties["overwrite"] = QJsonObject{
        {"type", "boolean"},
        {"description", "是否允许覆盖已有文件，默认 false"},
        {"default", false}
    };
    properties["encoding"] = QJsonObject{
        {"type", "string"},
        {"description", "文本编码，目前支持 UTF-8"},
        {"default", "UTF-8"}
    };

    schema["properties"] = properties;
    schema["required"] = QJsonArray{"path", "content"};
    return schema;
}

bool WriteTextFileTool::validate(const QJsonObject& params) const {
    return params.contains("path")
        && !params.value("path").toString().trimmed().isEmpty()
        && params.contains("content")
        && params.value("content").isString();
}

ToolResult WriteTextFileTool::execute(const QJsonObject& params) {
    const QString filePath = m_validator.normalizeFilePath(params.value("path").toString());
    const QString content = params.value("content").toString();
    const bool overwrite = params.value("overwrite").toBool(false);
    const QString encoding = params.value("encoding").toString("UTF-8").trimmed().toUpper();

    if (!m_validator.isPathAllowedForWrite(filePath)) {
        return ToolResult::fail(QString("路径不允许写入: %1").arg(filePath));
    }
    if (isSensitiveTarget(filePath)) {
        return ToolResult::fail(QString("目标文件类型或名称被安全策略禁止: %1").arg(filePath));
    }
    if (encoding != "UTF-8") {
        return ToolResult::fail("当前仅允许 UTF-8 写入，避免编码混淆");
    }

    const QByteArray bytes = content.toUtf8();
    if (bytes.size() > m_maxWriteBytes) {
        return ToolResult::fail(QString("写入内容过大，最大允许 %1 字节").arg(m_maxWriteBytes));
    }

    QFileInfo info(filePath);
    if (info.exists() && !overwrite) {
        return ToolResult::fail(QString("文件已存在；如确需覆盖，请显式设置 overwrite=true: %1").arg(filePath));
    }

    QDir parentDir = info.absoluteDir();
    if (!parentDir.exists() && !parentDir.mkpath(".")) {
        return ToolResult::fail(QString("无法创建父目录: %1").arg(parentDir.absolutePath()));
    }

    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return ToolResult::fail(QString("无法打开文件写入: %1").arg(filePath));
    }
    if (file.write(bytes) != bytes.size()) {
        return ToolResult::fail(QString("文件写入不完整: %1").arg(filePath));
    }
    if (!file.commit()) {
        return ToolResult::fail(QString("提交文件写入失败: %1").arg(filePath));
    }

    QJsonObject result;
    result["path"] = filePath;
    result["bytes_written"] = bytes.size();
    result["overwritten"] = info.exists();
    return ToolResult::ok(result);
}

bool WriteTextFileTool::isSensitiveTarget(const QString& filePath) const {
    const QFileInfo info(filePath);
    const QString name = info.fileName().toLower();
    const QString suffix = info.suffix().toLower();

    if (name == ".env" || name.contains("secret") || name.contains("token") || name.contains("password")) {
        return true;
    }

    static const QStringList blockedSuffixes = {
        "exe", "dll", "bat", "cmd", "ps1", "vbs", "js", "jar", "msi",
        "pem", "key", "pfx", "p12", "crt", "cer"
    };
    return blockedSuffixes.contains(suffix, Qt::CaseInsensitive);
}

// ================================================================
// ExecuteWhitelistedCommandTool 实现
// ================================================================

ExecuteWhitelistedCommandTool::ExecuteWhitelistedCommandTool(const CommandExecutionPolicy& policy)
    : AITool(
        "execute_whitelisted_command",
        "在用户授权的安全目录内执行白名单命令。命令必须精确匹配配置白名单；不会打开通用 shell。",
        ToolCategory::Action
      )
    , m_policy(policy)
    , m_validator(policy.allowedRoots) {}

QJsonObject ExecuteWhitelistedCommandTool::parameterSchema() const {
    QJsonObject schema;
    schema["type"] = "object";

    QJsonObject properties;
    properties["working_directory"] = QJsonObject{
        {"type", "string"},
        {"description", "命令工作目录，必须位于安全目录内"}
    };
    properties["command"] = QJsonObject{
        {"type", "string"},
        {"description", "要执行的程序名或绝对路径，必须精确匹配白名单"}
    };
    properties["args"] = QJsonObject{
        {"type", "array"},
        {"description", "命令参数数组；每个元素会作为独立参数传递，不经过 shell 拼接"},
        {"items", QJsonObject{{"type", "string"}}}
    };

    schema["properties"] = properties;
    schema["required"] = QJsonArray{"working_directory", "command"};
    return schema;
}

bool ExecuteWhitelistedCommandTool::validate(const QJsonObject& params) const {
    return params.contains("working_directory")
        && !params.value("working_directory").toString().trimmed().isEmpty()
        && params.contains("command")
        && !params.value("command").toString().trimmed().isEmpty();
}

ToolResult ExecuteWhitelistedCommandTool::execute(const QJsonObject& params) {
    const QString workingDirectory = m_validator.normalizeFilePath(params.value("working_directory").toString());
    const QString command = params.value("command").toString().trimmed();

    if (!m_validator.isPathAllowed(workingDirectory)) {
        return ToolResult::fail(QString("工作目录不在安全目录内: %1").arg(workingDirectory));
    }
    if (!QFileInfo(workingDirectory).isDir()) {
        return ToolResult::fail(QString("工作目录不存在或不是目录: %1").arg(workingDirectory));
    }
    if (!isCommandAllowed(command)) {
        return ToolResult::fail(QString("命令未列入白名单: %1").arg(command));
    }
    if (isShellProgram(command)) {
        return ToolResult::fail(QString("禁止执行通用 shell 或脚本解释器: %1").arg(command));
    }

    const QStringList args = parseArguments(params);
    QString unsafeReason;
    if (!areArgumentsSafe(args, workingDirectory, unsafeReason)) {
        return ToolResult::fail(unsafeReason);
    }

    QProcess process;
    process.setWorkingDirectory(workingDirectory);
    process.setProgram(command);
    process.setArguments(args);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();

    if (!process.waitForStarted(1000)) {
        return ToolResult::fail(QString("命令启动失败: %1").arg(process.errorString()));
    }

    const int timeoutMs = qBound(500, m_policy.timeoutMs, 30000);
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        return ToolResult::fail(QString("命令执行超时，已终止: %1 ms").arg(timeoutMs));
    }

    const int exitCode = process.exitCode();
    const QString stdoutText = limitedOutput(process.readAllStandardOutput());
    const QString stderrText = limitedOutput(process.readAllStandardError());

    QJsonObject result;
    result["working_directory"] = workingDirectory;
    result["command"] = command;
    result["exit_code"] = exitCode;
    result["stdout"] = stdoutText;
    result["stderr"] = stderrText;
    result["timed_out"] = false;

    if (exitCode != 0) {
        return ToolResult{false, result, QString("命令退出码非 0: %1").arg(exitCode)};
    }
    return ToolResult::ok(result);
}

bool ExecuteWhitelistedCommandTool::isCommandAllowed(const QString& command) const {
    for (const QString& allowed : m_policy.commandWhitelist) {
        if (command.compare(allowed.trimmed(), Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

bool ExecuteWhitelistedCommandTool::isShellProgram(const QString& command) const {
    const QString base = QFileInfo(command).fileName().toLower();
    static const QStringList blocked = {
        "cmd", "cmd.exe", "powershell", "powershell.exe", "pwsh", "pwsh.exe",
        "sh", "sh.exe", "bash", "bash.exe", "wscript", "wscript.exe", "cscript", "cscript.exe",
        "python", "python.exe", "py", "py.exe", "node", "node.exe", "npm", "npm.cmd", "npx", "npx.cmd",
        "perl", "perl.exe", "ruby", "ruby.exe", "java", "java.exe", "javaw", "javaw.exe"
    };
    return blocked.contains(base);
}

QStringList ExecuteWhitelistedCommandTool::parseArguments(const QJsonObject& params) const {
    QStringList args;
    const QJsonArray arr = params.value("args").toArray();
    for (const QJsonValue& value : arr) {
        if (value.isString()) {
            args.append(value.toString());
        }
    }
    return args;
}

bool ExecuteWhitelistedCommandTool::areArgumentsSafe(const QStringList& args,
                                                     const QString& workingDirectory,
                                                     QString& reason) const {
    if (args.size() > 16) {
        reason = "命令参数过多，已被安全策略拒绝";
        return false;
    }

    const QRegularExpression controlChars(QStringLiteral("[\\x00-\\x1F]"));
    static const QStringList suspiciousTokens = {
        "&&", "||", ";", "|", ">", "<", "`", "$(", "%COMSPEC%"
    };

    for (const QString& arg : args) {
        if (arg.size() > 256) {
            reason = QString("命令参数过长，已被安全策略拒绝: %1").arg(arg.left(40));
            return false;
        }
        if (controlChars.match(arg).hasMatch()) {
            reason = "命令参数包含控制字符，已被安全策略拒绝";
            return false;
        }
        for (const QString& token : suspiciousTokens) {
            if (arg.contains(token, Qt::CaseInsensitive)) {
                reason = QString("命令参数包含可疑 shell 片段，已被拒绝: %1").arg(token);
                return false;
            }
        }

        QString candidatePath;
        if (isPathLikeArgument(arg, &candidatePath)) {
            const QFileInfo info(candidatePath);
            const QString normalized = info.isAbsolute()
                ? m_validator.normalizeFilePath(candidatePath)
                : m_validator.normalizeFilePath(QDir(workingDirectory).filePath(candidatePath));
            if (!m_validator.isPathAllowed(normalized)) {
                reason = QString("命令参数中的路径不在安全目录内: %1").arg(arg);
                return false;
            }
        }
    }

    return true;
}

bool ExecuteWhitelistedCommandTool::isPathLikeArgument(const QString& arg, QString* candidatePath) const {
    QString value = arg.trimmed();
    const int equalIndex = value.indexOf('=');
    if (equalIndex >= 0 && equalIndex + 1 < value.size()) {
        value = value.mid(equalIndex + 1);
    }

    const bool pathLike = value.contains('/')
        || value.contains('\\')
        || value.startsWith(".")
        || QDir::isAbsolutePath(value);

    if (pathLike && candidatePath) {
        *candidatePath = value;
    }
    return pathLike;
}

QString ExecuteWhitelistedCommandTool::limitedOutput(const QByteArray& output) const {
    const int maxBytes = qBound(1024, m_policy.maxOutputBytes, 128 * 1024);
    QByteArray limited = output.left(maxBytes);
    QString text = QString::fromUtf8(limited);
    if (output.size() > maxBytes) {
        text.append(QString("\n...[output truncated at %1 bytes]").arg(maxBytes));
    }
    return text;
}
