#include "schema_validator.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QUrl>
#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>

namespace {
using Json = jsoncons::json;
using namespace jsoncons::jsonschema;
constexpr auto dialect = "https://json-schema.org/draft/2020-12/schema";
QByteArray encode(const QJsonValue& value) {
    auto bytes=QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return bytes.mid(1,bytes.size()-2);
}
void checkKeywords(const Json& schema) {
    if (schema.is_bool()) return;
    if (!schema.is_object()) throw std::runtime_error("Schema must be object or boolean");
    static const QSet<QString> known{"$schema","$id","$ref","$anchor","$dynamicRef","$dynamicAnchor","$defs","$vocabulary","$comment",
        "type","enum","const","multipleOf","maximum","exclusiveMaximum","minimum","exclusiveMinimum","maxLength","minLength","pattern",
        "maxItems","minItems","uniqueItems","maxContains","minContains","maxProperties","minProperties","required","dependentRequired",
        "allOf","anyOf","oneOf","not","if","then","else","properties","patternProperties","additionalProperties","propertyNames",
        "dependentSchemas","items","prefixItems","contains","unevaluatedItems","unevaluatedProperties","title","description","default","deprecated",
        "readOnly","writeOnly","examples","format"};
    static const QSet<QString> single{"not","if","then","else","additionalProperties","propertyNames","items","contains","unevaluatedItems","unevaluatedProperties"};
    static const QSet<QString> arrays{"allOf","anyOf","oneOf","prefixItems"};
    static const QSet<QString> maps{"$defs","properties","patternProperties","dependentSchemas"};
    for (const auto& item : schema.object_range()) {
        const auto key=QString::fromStdString(std::string(item.key()));
        if (!known.contains(key)) throw std::runtime_error("Unsupported schema keyword");
        if (key=="$schema" && item.value().as<std::string>() != dialect) throw std::runtime_error("Unsupported schema dialect");
        if (key=="$vocabulary") {
            for (const auto& vocabulary : item.value().object_range()) {
                const auto name=QString::fromStdString(std::string(vocabulary.key()));
                if (vocabulary.value().as<bool>() && !QSet<QString>{"https://json-schema.org/draft/2020-12/vocab/core","https://json-schema.org/draft/2020-12/vocab/applicator","https://json-schema.org/draft/2020-12/vocab/validation","https://json-schema.org/draft/2020-12/vocab/meta-data","https://json-schema.org/draft/2020-12/vocab/unevaluated","https://json-schema.org/draft/2020-12/vocab/format-annotation"}.contains(name))
                    throw std::runtime_error("Unsupported vocabulary");
            }
        }
        if (single.contains(key)) checkKeywords(item.value());
        else if (arrays.contains(key)) { for (const auto& child : item.value().array_range()) checkKeywords(child); }
        else if (maps.contains(key)) { for (const auto& child : item.value().object_range()) checkKeywords(child.value()); }
    }
}

QJsonValue atPointer(QJsonValue value, const QString& pointer) {
    if (pointer.isEmpty()) return value;
    if (!pointer.startsWith('/')) return QJsonValue::Undefined;
    for (QString token : pointer.mid(1).split('/')) {
        token.replace("~1", "/").replace("~0", "~");
        if (value.isObject()) value = value.toObject().value(token);
        else if (value.isArray()) {
            bool ok = false;
            const auto index = token.toInt(&ok);
            value = ok && index >= 0 ? value.toArray().at(index) : QJsonValue::Undefined;
        } else return QJsonValue::Undefined;
        if (value.isUndefined()) return value;
    }
    return value;
}

QJsonObject validationDetails(const validation_message& message,
                              const QJsonValue& schema, const QJsonValue& instance) {
    // Evaluation paths can append an unknown property from the input. The
    // schema location names the rule itself; publish it only if it resolves
    // in the supplied schema. Never expose instance locations or messages.
    QString path = QString::fromStdString(message.schema_location().fragment());
    auto rule = atPointer(schema, path);
    const QString keyword = QString::fromStdString(message.keyword());
    // Some validators point at the containing schema rather than the keyword.
    if (rule.isObject() && rule.toObject().contains(keyword)) {
        path += '/' + keyword;
        rule = rule.toObject()[keyword];
    }
    QJsonObject details{{"stage", "schema"}};
    if (path.size() <= 256 && !rule.isUndefined()) {
        details["schemaPath"] = path;
        details["keyword"] = keyword.left(64);
        if (message.keyword() == "required") {
            const auto object = atPointer(instance,
                QString::fromStdString(message.instance_location().string())).toObject();
            QJsonArray missing;
            for (const auto& field : rule.toArray()) {
                if (field.isString() && !object.contains(field.toString()))
                    missing.append(field.toString().left(128));
                if (missing.size() == 8) break;
            }
            if (!missing.isEmpty()) details["missingRequired"] = missing;
        }
    }
    return details;
}

Result<void,DomainError> evaluate(const QJsonValue& value, const QJsonValue* instance, const QString& root, const QString& sourceFile) {
    try {
        auto schema=Json::parse(encode(value).toStdString());
        checkKeywords(schema);
        static const auto meta=make_json_schema(meta_resolver<Json>(jsoncons::uri(dialect)),evaluation_options{}.default_version(dialect));
        if (!meta.is_valid(schema)) throw std::runtime_error("Invalid schema against 2020-12 metaschema");
        const QString base=sourceFile.isEmpty() ? QStringLiteral("urn:desktop-pet:parameters") : QUrl::fromLocalFile(sourceFile).toString();
        auto resolver=[root](const jsoncons::uri& uri) -> Json {
            const QUrl url(QString::fromStdString(uri.string()));
            if (!url.isLocalFile() || root.isEmpty()) throw std::runtime_error("Remote schema reference is not allowed");
            const auto resolved=SchemaValidator::packagePath(root,QDir(root).relativeFilePath(url.toLocalFile()));
            if (!resolved) throw std::runtime_error("Schema reference escapes package");
            QFile file(resolved.value());
            if (!file.open(QIODevice::ReadOnly) || file.size()>2*1024*1024) throw std::runtime_error("Schema reference unavailable");
            auto result=Json::parse(file.readAll().toStdString()); checkKeywords(result);
            if (!meta.is_valid(result)) throw std::runtime_error("Invalid referenced schema");
            return result;
        };
        auto compiled=make_json_schema(schema,base.toStdString(),resolver,evaluation_options{}.default_version(dialect));
        if (instance) {
            QJsonObject details;
            compiled.validate(Json::parse(encode(*instance).toStdString()),
                [&](const validation_message& message) -> walk_result {
                    details = validationDetails(message, value, *instance);
                    return walk_result::abort;
                });
            if (!details.isEmpty())
                return Result<void,DomainError>::failure(
                    domainError("INVALID_INPUT", "JSON Schema validation failed", details));
        }
        return Result<void,DomainError>::success();
    } catch (const std::exception&) {
        // Never return schema or configuration values in diagnostics.
        return Result<void,DomainError>::failure(domainError("SCHEMA_UNSUPPORTED","Invalid or unsupported JSON Schema/reference",
                                                            {{"stage", "schema"}, {"reason", "unsupported_schema"}}));
    }
}
}
Result<void,DomainError> SchemaValidator::check(const QJsonValue& schema,const QString& root,const QString& file) { return evaluate(schema,nullptr,root,file); }
Result<void,DomainError> SchemaValidator::validate(const QJsonValue& schema,const QJsonValue& value,const QString& root,const QString& file) { return evaluate(schema,&value,root,file); }
Result<void,DomainError> SchemaValidator::validateManifest(const QString& kind,const QJsonObject& instance) {
    // Explicit reference keeps resources in the static library linked.
    Q_INIT_RESOURCE(plugin_contracts);
    QFile file(":/plugin-contracts/"+kind+".schema.json");
    if (!file.open(QIODevice::ReadOnly)) return Result<void,DomainError>::failure(domainError("DEPENDENCY_UNAVAILABLE","Manifest schema unavailable"));
    return validate(QJsonDocument::fromJson(file.readAll()).object(),instance);
}
Result<QString,DomainError> SchemaValidator::packagePath(const QString& root,const QString& relative,const QString& baseDirectory) {
    const QString canonicalRoot=QFileInfo(root).canonicalFilePath();
    const QString base=baseDirectory.isEmpty()?canonicalRoot:baseDirectory;
    const QString path=QFileInfo(QDir(base).filePath(relative)).canonicalFilePath();
    if (canonicalRoot.isEmpty() || QDir::isAbsolutePath(relative) || path.isEmpty()
        || !path.startsWith(canonicalRoot+'/') || !QFileInfo(path).isFile())
        return Result<QString,DomainError>::failure(domainError("INVALID_INPUT","Package reference is unavailable or outside its root"));
    return Result<QString,DomainError>::success(path);
}
