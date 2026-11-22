#pragma once

#include "storage/AppSettings.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>

class WebDavClient;

// Move this object to a QThread and invoke start() with a queued connection.
// The operation itself is synchronous and uses WebDavClient's local event loop.
class RemoteCatalogWorker : public QObject
{
    Q_OBJECT

public:
    explicit RemoteCatalogWorker(QObject *parent = nullptr);
    ~RemoteCatalogWorker() override;

    void configure(const SyncSettings &baseSettings, const QString &serverPassword);
    void setRequestTimeout(int milliseconds);

public slots:
    void start();
    void cancel();

signals:
    void progress(const QString &message);
    void finished(bool success, const QString &message, const QStringList &notebookIds);

private:
    WebDavClient *m_client;
    SyncSettings m_settings;
    QString m_serverPassword;
    std::atomic_bool m_cancelled{false};
    std::atomic_bool m_running{false};
};
