#include "runtime_unit_of_work.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

#include <utility>

SqliteRuntimeUnitOfWork::SqliteRuntimeUnitOfWork(QString connectionName)
    : m_connectionName(std::move(connectionName)) {}

Result<std::unique_ptr<RuntimeUnitOfWork>, DomainError>
SqliteRuntimeUnitOfWork::begin(const QString& databasePath) {
    if (databasePath.trimmed().isEmpty()) {
        return Result<std::unique_ptr<RuntimeUnitOfWork>, DomainError>::failure(
            domainError(QStringLiteral("EVENT_OUTBOX_UNAVAILABLE"),
                        QStringLiteral("runtime database path is empty")));
    }
    const QString name = QStringLiteral("runtime_uow_%1").arg(
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
    database.setDatabaseName(databasePath);
    if (!database.open()) {
        const DomainError error = domainError(
            QStringLiteral("EVENT_OUTBOX_UNAVAILABLE"), database.lastError().text());
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(name);
        return Result<std::unique_ptr<RuntimeUnitOfWork>, DomainError>::failure(error);
    }
    QSqlQuery pragma(database);
    if (!pragma.exec(QStringLiteral("PRAGMA foreign_keys=ON"))
        || !pragma.exec(QStringLiteral("PRAGMA busy_timeout=5000"))
        || !database.transaction()) {
        const DomainError error = domainError(
            QStringLiteral("EVENT_OUTBOX_UNAVAILABLE"),
            QStringLiteral("failed to begin runtime unit of work: %1")
                .arg(database.lastError().text()));
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(name);
        return Result<std::unique_ptr<RuntimeUnitOfWork>, DomainError>::failure(error);
    }
    return Result<std::unique_ptr<RuntimeUnitOfWork>, DomainError>::success(
        std::unique_ptr<RuntimeUnitOfWork>(new SqliteRuntimeUnitOfWork(name)));
}

SqliteRuntimeUnitOfWork::~SqliteRuntimeUnitOfWork() {
    rollback();
    closeConnection();
}

Result<void, DomainError> SqliteRuntimeUnitOfWork::commit() {
    if (!m_active || !QSqlDatabase::contains(m_connectionName)) {
        return Result<void, DomainError>::failure(
            domainError(QStringLiteral("STATE_VERSION_CONFLICT"),
                        QStringLiteral("runtime unit of work is no longer active")));
    }
    QSqlDatabase database = QSqlDatabase::database(m_connectionName);
    if (!database.commit()) {
        database.rollback();
        m_active = false;
        return Result<void, DomainError>::failure(
            domainError(QStringLiteral("EVENT_OUTBOX_UNAVAILABLE"),
                        QStringLiteral("failed to commit runtime unit of work: %1")
                            .arg(database.lastError().text())));
    }
    m_active = false;
    return Result<void, DomainError>::success();
}

void SqliteRuntimeUnitOfWork::rollback() {
    if (!m_active || !QSqlDatabase::contains(m_connectionName)) return;
    QSqlDatabase::database(m_connectionName).rollback();
    m_active = false;
}

void SqliteRuntimeUnitOfWork::closeConnection() {
    if (!QSqlDatabase::contains(m_connectionName)) return;
    {
        QSqlDatabase database = QSqlDatabase::database(m_connectionName, false);
        if (database.isValid()) database.close();
    }
    QSqlDatabase::removeDatabase(m_connectionName);
}

SqliteRuntimeUnitOfWorkFactory::SqliteRuntimeUnitOfWorkFactory(QString databasePath)
    : m_databasePath(std::move(databasePath)) {}

Result<std::unique_ptr<RuntimeUnitOfWork>, DomainError>
SqliteRuntimeUnitOfWorkFactory::begin() {
    return SqliteRuntimeUnitOfWork::begin(m_databasePath);
}
