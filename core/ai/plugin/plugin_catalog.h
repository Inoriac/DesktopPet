#pragma once
#include "capability_types.h"
#include <QHash>
#include <QMutex>

// Rebuildable index. Locks protect snapshots only; no executable/network I/O.
class PluginCatalog {
public:
    Result<PluginRegistration,DomainError> registerPlugin(const PluginLocation& location);
    Result<LoadedCapability,DomainError> loadCapability(const CapabilityRef& ref) const;
    Result<void,DomainError> setPluginEnabled(const PluginStateChange& change);
    QJsonArray discover() const;
    quint64 revision() const;
    void invalidate();
    bool enabled(const QString& pluginId) const;
private:
    struct Entry { QJsonObject summary; QString path; QStringList operationIds; QJsonArray dependencies; };
    struct Package { QString root, digest; QStringList entryIds; bool enabled=true; };
    static Result<QString,DomainError> digestPackage(const QString& root);
    mutable QMutex m_mutex;
    QHash<QString,Entry> m_entries;
    QHash<QString,Package> m_packages;
    quint64 m_revision=1;
};
