#pragma once

#include "storage/AppSettings.h"

#include <QByteArray>
#include <QObject>
#include <QStringList>
#include <QUrl>

#include <atomic>

class QNetworkAccessManager;
class QNetworkReply;

struct WebDavResponse
{
    int statusCode = 0;
    QByteArray body;
    QString errorString;
    QUrl finalUrl;

    bool hasHttpStatus() const { return statusCode > 0; }
    bool isSuccess() const
    {
        return errorString.isEmpty() && statusCode >= 200 && statusCode < 300;
    }
};

// Synchronous WebDAV operations intended to run in a worker thread. Each call
// uses a local event loop, so no GUI or application-wide event loop is needed.
class WebDavClient : public QObject
{
    Q_OBJECT

public:
    explicit WebDavClient(QObject *parent = nullptr);
    ~WebDavClient() override;

    void configure(const SyncSettings &settings, const QString &serverPassword);
    void setTimeout(int milliseconds);
    int timeout() const { return m_timeoutMs; }

    bool isConfigured(QString *error = nullptr) const;
    QUrl remoteUrl(const QString &relativePath = QString()) const;

    WebDavResponse get(const QString &relativePath);
    WebDavResponse put(const QString &relativePath, const QByteArray &data);
    WebDavResponse propFind(const QString &relativePath = QString());
    WebDavResponse mkCol(const QString &relativePath = QString());

    // Returns the URL-decoded names of immediate child collections. The
    // configured collection itself and non-collection resources are excluded.
    QStringList listCollections(const QString &relativePath = QString(), QString *error = nullptr);

    // Creates the configured remote collection and all missing parents.
    bool ensureCollection(const QString &relativePath = QString(), QString *error = nullptr);

    // Thread-safe cancellation. An active request polls this flag and aborts.
    void cancel();
    void resetCancellation();

private:
    WebDavResponse request(const QByteArray &method, const QUrl &url,
                           const QByteArray &data = QByteArray(),
                           const QByteArray &contentType = QByteArray(),
                           int depth = 0);
    QUrl endpointUrl(QString *error = nullptr) const;
    static bool appendPath(const QUrl &base, const QString &path, QUrl *result);
    static QStringList pathSegments(const QString &path, bool *ok);
    static bool responseError(const WebDavResponse &response, const QString &action,
                              QString *error);

    SyncSettings m_settings;
    QByteArray m_serverPassword;
    QNetworkAccessManager *m_network = nullptr;
    int m_timeoutMs = 30000;
    std::atomic_bool m_cancelled{false};
};
