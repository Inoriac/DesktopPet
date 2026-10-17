#include "mcp_client.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
McpClient::~McpClient() { close(); }
void McpClient::close() {
    if (m_process.state()!=QProcess::NotRunning) { m_process.kill(); m_process.waitForFinished(1000); }
    m_connected=false;
    m_tools.clear();
    m_buffer.clear();
}
Result<QJsonObject,DomainError> McpClient::request(const QString& method,const QJsonObject& params,int timeoutMs) {
    using R=Result<QJsonObject,DomainError>;
    const qint64 id=++m_sequence;
    const QByteArray payload=QJsonDocument(QJsonObject{{"jsonrpc","2.0"},{"id",id},{"method",method},{"params",params}}).toJson(QJsonDocument::Compact)+'\n';
    if (m_process.state()!=QProcess::Running || m_process.write(payload)!=payload.size()) return R::failure(domainError("MCP_DISCONNECTED","MCP transport is unavailable"));
    QElapsedTimer elapsed; elapsed.start(); qint64 bytes=0;
    while (elapsed.elapsed()<timeoutMs) {
        m_process.waitForReadyRead(qMin(50,timeoutMs-int(elapsed.elapsed())));
        m_buffer+=m_process.readAllStandardOutput(); bytes+=m_process.readAllStandardError().size();
        if (m_buffer.size()+bytes>2*1024*1024) return R::failure(domainError("MCP_PROTOCOL_ERROR","MCP response limit exceeded"));
        while (m_buffer.contains('\n')) {
            const int end=m_buffer.indexOf('\n'); const auto line=m_buffer.left(end); m_buffer.remove(0,end+1);
            QJsonParseError error; const auto doc=QJsonDocument::fromJson(line,&error);
            if (error.error!=QJsonParseError::NoError || !doc.isObject()) return R::failure(domainError("MCP_PROTOCOL_ERROR","Malformed MCP protocol response"));
            const auto response=doc.object();
            if (response["jsonrpc"]!="2.0") return R::failure(domainError("MCP_PROTOCOL_ERROR","Invalid JSON-RPC version"));
            if (response.contains("method")) {
                // Notifications carry no model instructions. Server requests are explicitly unsupported.
                if (response.contains("id")) m_process.write(QJsonDocument(QJsonObject{{"jsonrpc","2.0"},{"id",response["id"]},{"error",QJsonObject{{"code",-32601},{"message","Host method unavailable"}}}}).toJson(QJsonDocument::Compact)+'\n');
                continue;
            }
            if (response["id"].toInteger(-1)!=id) return R::failure(domainError("MCP_PROTOCOL_ERROR","Unexpected MCP response identity"));
            if (response.contains("error")) return R::failure(domainError("MCP_PROTOCOL_ERROR","MCP request returned a protocol error"));
            if (!response["result"].isObject()) return R::failure(domainError("MCP_PROTOCOL_ERROR","MCP result is not an object"));
            return R::success(response["result"].toObject());
        }
        if (m_process.state()==QProcess::NotRunning) { m_connected=false; return R::failure(domainError("MCP_DISCONNECTED","MCP process disconnected")); }
    }
    return R::failure(domainError("MCP_TIMEOUT","MCP request timed out"));
}
Result<void,DomainError> McpClient::connectStdio(const QString& program,const QStringList& args,int timeoutMs) {
    using R=Result<void,DomainError>; close();
    QElapsedTimer elapsed; elapsed.start();
    m_process.setProcessChannelMode(QProcess::SeparateChannels); m_process.start(program,args);
    if (!m_process.waitForStarted(qMin(timeoutMs,3000))) return R::failure(domainError("DEPENDENCY_UNAVAILABLE","MCP process could not start"));
    auto remaining=[&](){ return qMax(1,timeoutMs-int(elapsed.elapsed())); };
    auto initialized=request("initialize",{{"protocolVersion","2024-11-05"},{"capabilities",QJsonObject{}},{"clientInfo",QJsonObject{{"name","DesktopPet"},{"version","1"}}}},remaining());
    if (!initialized) { close(); return R::failure(initialized.error()); }
    const QString version=initialized.value()["protocolVersion"].toString();
    if (!QStringList{"2024-11-05","2025-03-26","2025-06-18"}.contains(version) || !initialized.value()["capabilities"].toObject()["tools"].isObject()) {
        close(); return R::failure(domainError("MCP_PROTOCOL_ERROR","Unsupported MCP initialization"));
    }
    m_process.write("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n");
    QString cursor; QSet<QString> cursors,names;
    for (int page=0;page<100;++page) {
        if (elapsed.elapsed()>=timeoutMs) { close(); return R::failure(domainError("MCP_TIMEOUT","MCP enumeration timed out")); }
        auto list=request("tools/list",cursor.isEmpty()?QJsonObject{}:QJsonObject{{"cursor",cursor}},remaining());
        if (!list) { close(); return R::failure(list.error()); }
        if (!list.value()["tools"].isArray()) { close(); return R::failure(domainError("MCP_PROTOCOL_ERROR","MCP tools list is invalid")); }
        for (const auto& value:list.value()["tools"].toArray()) {
            const auto tool=value.toObject(); const auto name=tool["name"].toString();
            if (name.isEmpty() || names.contains(name) || !tool["inputSchema"].isObject()) { close(); return R::failure(domainError("MCP_PROTOCOL_ERROR","MCP tool descriptor is invalid")); }
            names.insert(name); m_tools.append({name,tool["description"].toString(),tool["inputSchema"].toObject()});
        }
        cursor=list.value()["nextCursor"].toString();
        if (cursor.isEmpty()) { m_connected=true; return R::success(); }
        if (cursors.contains(cursor)) break;
        cursors.insert(cursor);
    }
    close(); return R::failure(domainError("MCP_PROTOCOL_ERROR","MCP pagination did not terminate"));
}
Result<QJsonObject,DomainError> McpClient::callTool(const QString& name,const QJsonObject& args,int timeoutMs) {
    if (!m_connected) return Result<QJsonObject,DomainError>::failure(domainError("DEPENDENCY_UNAVAILABLE","MCP initialization is incomplete"));
    bool found=false; for (const auto& tool:m_tools) found |= tool.name==name;
    if (!found) return Result<QJsonObject,DomainError>::failure(domainError("UNKNOWN_REFERENCE","MCP tool is not advertised"));
    return request("tools/call",{{"name",name},{"arguments",args}},timeoutMs);
}
