#include "capability_executor.h"
#include "schema_validator.h"
#include "ai/mcp/mcp_client.h"
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QDir>

namespace {
using ValueResult=Result<QJsonValue,DomainError>;
CapabilityOutcome failed(const CapabilityCall& call,const DomainError& error,bool executed=false,bool read=true) {
    CapabilityOutcome out; out.callId=call.callId; out.activityId=call.activityId; out.error=error; out.executed=executed;
    if (executed && !read) { out.stage="unknown"; out.effectCertainty="unknown"; }
    return out;
}
Result<void,DomainError> connect(McpClient& client,const LoadedCapability& loaded,const CapabilityBinding& binding,int timeout) {
    const auto invocation=loaded.operation["invocation"].toObject();
    const QString connection=binding.configuration[invocation["connection"].toObject()["setting"].toString()].toString();
    const auto config=binding.connections[connection].toObject();
    QStringList args; for (const auto& v:config["arguments"].toArray()) { if (!v.isString()) return Result<void,DomainError>::failure(domainError("INVALID_INPUT","Invalid MCP host binding")); args.append(v.toString()); }
    const auto program=config["program"].toString();
    if (!QDir::isAbsolutePath(program) || !QFileInfo(program).isExecutable()) return Result<void,DomainError>::failure(domainError("DEPENDENCY_UNAVAILABLE","MCP executable is unavailable"));
    return client.connectStdio(program,args,timeout);
}
Result<QJsonObject,DomainError> actualSchema(McpClient& client,const LoadedCapability& loaded) {
    const auto name=loaded.operation["invocation"].toObject()["tool"].toString();
    for (const auto& tool:client.tools()) if (tool.name==name) {
        auto valid=SchemaValidator::check(tool.inputSchema);
        if (!valid) return Result<QJsonObject,DomainError>::failure(valid.error());
        return Result<QJsonObject,DomainError>::success(tool.inputSchema);
    }
    return Result<QJsonObject,DomainError>::failure(domainError("UNKNOWN_REFERENCE","Configured MCP tool is not advertised"));
}
bool usable(const QJsonValue& value) {
    return !value.isNull() && !value.isUndefined() && (!value.isString() || !value.toString().trimmed().isEmpty())
        && (!value.isObject() || !value.toObject().isEmpty()) && (!value.isArray() || !value.toArray().isEmpty());
}
CapabilityOutcome interpret(const CapabilityCall& call,const LoadedCapability& loaded,const QJsonValue& raw,const CapabilityBinding& binding) {
    const bool read=loaded.operation["effects"].toObject()["kind"]=="read";
    auto invalid=[&](const QString& message){ return failed(call,domainError("ACTION_OUTCOME_UNKNOWN",message),true,read); };
    const auto result=loaded.operation["result"].toObject(); const auto mapping=binding.resultAdapter;
    auto data=CapabilityExecutor::pointer(raw,mapping["dataPointer"].toString());
    if (!data) return invalid("Result data mapping is unavailable");
    if (result.contains("schema")) {
        auto valid=SchemaValidator::validate(result["schema"],data.value(),loaded.rootPath,loaded.definitionPath);
        if (!valid) return invalid("Result does not match the declared schema");
    }
    CapabilityOutcome out; out.callId=call.callId; out.activityId=call.activityId; out.executed=true; out.data=data.value();
    out.stage=result["completion"].toString(); out.effectCertainty=read?"no_effect":"confirmed";
    if (out.stage=="reported") {
        if (!mapping.contains("statePointer") || !mapping["stateMap"].isObject()) return invalid("Reported result adapter is not configured");
        const auto state=CapabilityExecutor::pointer(raw,mapping["statePointer"].toString());
        if (!state || !state.value().isString()) return invalid("Reported result state is unavailable");
        out.stage=mapping["stateMap"].toObject()[state.value().toString()].toString();
    }
    if (!QStringList{"completed","accepted","failed","unknown"}.contains(out.stage)) return invalid("Reported result state is not recognized");
    auto mapped=[&](const QString& key)->QJsonValue { if (!mapping.contains(key)) return {}; auto value=CapabilityExecutor::pointer(raw,mapping[key].toString()); return value?value.value():QJsonValue{}; };
    out.receipt=mapped("receiptPointer"); out.tracking=mapped("trackingPointer");
    if (out.stage=="accepted" && (!usable(out.receipt) || !usable(out.tracking))) return invalid("Accepted result has no usable receipt and tracking method");
    if (usable(mapped("errorPointer"))) out.stage="failed";
    if (out.stage=="failed") { out.error=domainError("ACTION_FAILED","Operation reported failure"); out.effectCertainty=read?"no_effect":"unknown"; }
    if (out.stage=="unknown") { out.error=domainError("ACTION_OUTCOME_UNKNOWN","Operation outcome is unknown"); out.effectCertainty=read?"no_effect":"unknown"; }
    return out;
}
}
Result<QJsonValue,DomainError> CapabilityExecutor::pointer(const QJsonValue& value,const QString& pointer) {
    if (pointer.isEmpty()) return ValueResult::success(value);
    if (!pointer.startsWith('/')) return ValueResult::failure(domainError("INVALID_INPUT","Invalid JSON Pointer"));
    QJsonValue current=value;
    for (QString part:pointer.mid(1).split('/')) {
        for (int i=0;i<part.size();++i) if (part[i]=='~' && (i+1>=part.size() || (part[i+1]!='0' && part[i+1]!='1'))) return ValueResult::failure(domainError("INVALID_INPUT","Invalid JSON Pointer escape"));
        part.replace("~1","/"); part.replace("~0","~");
        if (current.isObject()) { auto object=current.toObject(); if (!object.contains(part)) return ValueResult::failure(domainError("INVALID_INPUT","JSON Pointer target is absent")); current=object[part]; }
        else if (current.isArray()) { bool ok=false; int index=part.toInt(&ok); auto array=current.toArray(); if (!ok || index<0 || index>=array.size() || QString::number(index)!=part) return ValueResult::failure(domainError("INVALID_INPUT","JSON Pointer array index is invalid")); current=array[index]; }
        else return ValueResult::failure(domainError("INVALID_INPUT","JSON Pointer target has the wrong type"));
    }
    return ValueResult::success(current);
}
Result<QJsonObject,DomainError> CapabilityExecutor::remoteSchema(const LoadedCapability& loaded,const CapabilityBinding& binding,int timeout) {
    McpClient client; auto connected=connect(client,loaded,binding,timeout);
    if (!connected) return Result<QJsonObject,DomainError>::failure(connected.error());
    return actualSchema(client,loaded);
}
CapabilityOutcome CapabilityExecutor::execute(const LoadedCapability& loaded,const CapabilityCall& call,const CapabilityBinding& binding,
                                                const std::function<Result<void,DomainError>(bool)>& beforeDispatch) {
    const bool read=loaded.operation["effects"].toObject()["kind"]=="read";
    auto fail=[&](const DomainError& error,bool executed=false){ return failed(call,error,executed,read); };
    const auto invocation=loaded.operation["invocation"].toObject();
    const auto parameters=loaded.operation["parameters"].toObject();
    if (parameters["source"]=="inline") {
        auto valid=SchemaValidator::validate(parameters["schema"],call.arguments,loaded.rootPath,loaded.definitionPath);
        if (!valid) return fail(valid.error());
    }
    QElapsedTimer elapsed; elapsed.start(); const int timeout=qBound(1,call.timeoutMs,300000);
    if (invocation["kind"]=="mcp") {
        auto allowed=beforeDispatch(invocation["kind"]!="mcp"); if (!allowed) return fail(allowed.error());
        McpClient client; auto connected=connect(client,loaded,binding,timeout);
        if (!connected) return fail(connected.error());
        auto schema=actualSchema(client,loaded); if (!schema) return fail(schema.error());
        auto valid=SchemaValidator::validate(schema.value(),call.arguments);
        if (!valid) return fail(valid.error());
        // Initialization has no business effect; recheck after it before tools/call.
        allowed=beforeDispatch(true); if (!allowed) return fail(allowed.error());
        if (elapsed.elapsed()>=timeout) return fail(domainError("MCP_TIMEOUT","MCP budget exhausted before call"));
        auto result=client.callTool(invocation["tool"].toString(),call.arguments,timeout-int(elapsed.elapsed()));
        if (!result) return fail(result.error(),true);
        if (result.value().contains("isError") && !result.value()["isError"].isBool()) return fail(domainError("MCP_PROTOCOL_ERROR","Invalid MCP tool error flag"),true);
        if (result.value()["isError"].toBool()) {
            auto out=fail(domainError("MCP_TOOL_ERROR","MCP tool reported failure"),true); out.stage="failed"; out.data=result.value(); return out;
        }
        return interpret(call,loaded,result.value(),binding);
    }
    const QString program=binding.configuration[invocation["program"].toObject()["setting"].toString()].toString();
    if (!QDir::isAbsolutePath(program) || !QFileInfo(program).isExecutable()) return fail(domainError("DEPENDENCY_UNAVAILABLE","Configured executable is unavailable"));
    QStringList arguments;
    for (const auto& value:invocation["arguments"].toArray()) {
        if (value.isString()) { arguments.append(value.toString()); continue; }
        const auto expression=value.toObject();
        QJsonValue item;
        if (expression.contains("setting")) item=binding.configuration[expression["setting"].toString()];
        else {
            auto resolved=pointer(call.arguments,expression.value(expression.contains("expand")?"expand":"input").toString());
            if (!resolved) return fail(resolved.error()); item=resolved.value();
        }
        if (expression.contains("expand")) {
            if (!item.isArray()) return fail(domainError("INVALID_INPUT","Expanded argument must be an array"));
            for (const auto& arg:item.toArray()) { if (!arg.isString()) return fail(domainError("INVALID_INPUT","Expanded argument must contain strings")); arguments.append(arg.toString()); }
        } else { if (!item.isString()) return fail(domainError("INVALID_INPUT","Argument binding must resolve to a string")); arguments.append(item.toString()); }
    }
    QProcess process; process.setProcessChannelMode(QProcess::SeparateChannels);
    if (invocation.contains("workingDirectory")) {
        auto directory=binding.configuration[invocation["workingDirectory"].toObject()["setting"].toString()];
        if (!directory.isString() || !QDir::isAbsolutePath(directory.toString()) || !QFileInfo(directory.toString()).isDir()) return fail(domainError("DEPENDENCY_UNAVAILABLE","Working directory is unavailable"));
        process.setWorkingDirectory(directory.toString());
    } else process.setWorkingDirectory(QDir::tempPath());
    auto allowed=beforeDispatch(invocation["kind"]!="mcp"); if (!allowed) return fail(allowed.error());
    process.start(program,arguments);
    if (!process.waitForStarted(qMin(3000,timeout))) return fail(domainError("DEPENDENCY_UNAVAILABLE","Executable failed to start"));
    if (invocation["stdin"]=="json") process.write(QJsonDocument(call.arguments).toJson(QJsonDocument::Compact));
    process.closeWriteChannel(); QByteArray output; qint64 errorBytes=0;
    while (process.state()!=QProcess::NotRunning && elapsed.elapsed()<timeout) {
        process.waitForFinished(qMin(25,timeout-int(elapsed.elapsed()))); output+=process.readAllStandardOutput(); errorBytes+=process.readAllStandardError().size();
        if (output.size()+errorBytes>2*1024*1024) break;
    }
    output+=process.readAllStandardOutput(); errorBytes+=process.readAllStandardError().size();
    if (process.state()!=QProcess::NotRunning) { process.kill(); process.waitForFinished(1000); return fail(domainError("ACTION_OUTCOME_UNKNOWN","Command timed out or exceeded output limit"),true); }
    if (output.size()+errorBytes>2*1024*1024) return fail(domainError("ACTION_OUTCOME_UNKNOWN","Command output limit exceeded"),true);
    if (process.exitStatus()!=QProcess::NormalExit || process.exitCode()!=0) return fail(domainError("ACTION_FAILED","Command exited unsuccessfully"),true);
    QJsonValue raw;
    if (loaded.operation["result"].toObject()["format"]=="text") raw=QString::fromUtf8(output);
    else {
        QJsonParseError error; const auto doc=QJsonDocument::fromJson('['+output+']',&error);
        if (error.error!=QJsonParseError::NoError || !doc.isArray() || doc.array().size()!=1) return fail(domainError("ACTION_OUTCOME_UNKNOWN","Command output is not one JSON value"),true);
        raw=doc.array()[0];
    }
    return interpret(call,loaded,raw,binding);
}
