#pragma once

#include "storage/AppSettings.h"

#include <QObject>
#include <QString>

#include <atomic>

class WebDavClient;

// Imports one encrypted remote notebook without registering it in AppSettings.
// Move this object to a QThread and invoke start() with a queued connection.
class RemoteImportWorker : public QObject
{
    Q_OBJECT

public:
    explicit RemoteImportWorker(QObject *parent = nullptr);
    ~RemoteImportWorker() override;

    void configure(const SyncSettings &baseSettings, const QString &serverPassword,
                   const QString &masterPassword, const QString &remoteNotebookId,
                   const QString &localRootPath, const QString &requestedLocalName,
                   const QString &writerId);
    void setRequestTimeout(int milliseconds);

public slots:
    void start();
    void cancel();

signals:
    void progress(const QString &message);
    void finished(bool success, const QString &message, const QString &localRootPath,
                  int importedObjects);

private:
    WebDavClient *m_client;
    SyncSettings m_settings;
    QString m_serverPassword;
    QString m_masterPassword;
    QString m_remoteNotebookId;
    QString m_localRootPath;
    QString m_requestedLocalName;
    QString m_writerId;
    std::atomic_bool m_cancelled{false};
    std::atomic_bool m_running{false};
};
