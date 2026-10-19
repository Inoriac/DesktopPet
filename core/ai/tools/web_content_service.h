#ifndef DESKTOP_PET_WEB_CONTENT_SERVICE_H
#define DESKTOP_PET_WEB_CONTENT_SERVICE_H

#include <QByteArray>
#include <QHostAddress>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QUrl>
#include <functional>

class NetworkSecurityValidator {
public:
    bool isUrlAllowed(const QString& url) const;
    bool isIpBlocked(const QString& host) const;
    bool resolveAllowedAddress(const QUrl& url, QHostAddress* address, QString* errorMessage = nullptr) const;
    static QList<QHostAddress> resolveHostAddresses(const QString& host);

private:
    bool isAddressBlocked(const QHostAddress& address) const;
    bool isBlockedIpv4(quint32 ip) const;
    bool isBlockedIpv6(const Q_IPV6ADDR& ip) const;
};

struct WebHttpResponse {
    bool success = false;
    QByteArray body, contentType;
    QUrl finalUrl;
    QString errorCode, errorMessage;
    int statusCode = 0;
};

struct WebContentResult {
    bool success = false;
    QJsonObject data;
    QString errorCode, errorMessage;
};

using WebHttpGet = std::function<WebHttpResponse(const QUrl&, int, qint64)>;

class WebContentService {
public:
    explicit WebContentService(QString cacheDir = {}, WebHttpGet transport = {});
    WebContentResult search(const QString& query, int limit = 5);
    WebContentResult readPage(const QString& url, const QString& cursor = {}, int timeoutMs = 15000);

private:
    QString m_cacheDir;
    WebHttpGet m_transport;
};

#endif
