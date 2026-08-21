#ifndef DESKTOP_PET_RUNTIME_UNIT_OF_WORK_H
#define DESKTOP_PET_RUNTIME_UNIT_OF_WORK_H

#include <memory>

#include "ai/domain/domain_result.h"

class RuntimeUnitOfWork {
public:
    virtual ~RuntimeUnitOfWork() = default;
    virtual QString connectionName() const = 0;
    virtual Result<void, DomainError> commit() = 0;
    virtual void rollback() = 0;
};

class RuntimeUnitOfWorkFactory {
public:
    virtual ~RuntimeUnitOfWorkFactory() = default;
    virtual Result<std::unique_ptr<RuntimeUnitOfWork>, DomainError> begin() = 0;
};

class SqliteRuntimeUnitOfWork final : public RuntimeUnitOfWork {
public:
    static Result<std::unique_ptr<RuntimeUnitOfWork>, DomainError> begin(
        const QString& databasePath);
    ~SqliteRuntimeUnitOfWork() override;

    QString connectionName() const override { return m_connectionName; }
    Result<void, DomainError> commit() override;
    void rollback() override;

private:
    explicit SqliteRuntimeUnitOfWork(QString connectionName);
    void closeConnection();

    QString m_connectionName;
    bool m_active = true;
};

class SqliteRuntimeUnitOfWorkFactory final : public RuntimeUnitOfWorkFactory {
public:
    explicit SqliteRuntimeUnitOfWorkFactory(QString databasePath);
    Result<std::unique_ptr<RuntimeUnitOfWork>, DomainError> begin() override;

private:
    QString m_databasePath;
};

#endif // DESKTOP_PET_RUNTIME_UNIT_OF_WORK_H
