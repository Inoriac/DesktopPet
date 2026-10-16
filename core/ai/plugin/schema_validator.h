#pragma once
#include "ai/domain/domain_result.h"
#include <QJsonValue>
class SchemaValidator {
public:
    static Result<void,DomainError> check(const QJsonValue& schema, const QString& root = {}, const QString& sourceFile = {});
    static Result<void,DomainError> validate(const QJsonValue& schema, const QJsonValue& instance,
                                            const QString& root = {}, const QString& sourceFile = {});
    static Result<void,DomainError> validateManifest(const QString& kind, const QJsonObject& instance);
    static Result<QString,DomainError> packagePath(const QString& root, const QString& relative, const QString& baseDirectory = {});
};
