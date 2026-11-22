#pragma once

#include "storage/AppSettings.h"

#include <QObject>
#include <QString>

#include <atomic>

class NotebookStore;
class WebDavClient;

// Move this object to a QThread and invoke start() with a queued connection.
// Passwords are supplied by the caller; persistence is handled by AppSettings.
class SyncWorker : public QObject
{
    Q_OBJECT

public:
    explicit SyncWorker(NotebookStore *store, QObject *parent = nullptr);
    ~SyncWorker() override;

    void configure(const SyncSettings &settings, const QString &serverPassword,
                   const QString &masterPassword);
    void setRequestTimeout(int milliseconds);

public slots:
    void start();
    void cancel();

signals:
    void progress(const QString &message);
    void finished(bool success, const QString &message, int uploaded, int downloaded);

private:
    NotebookStore *m_store;
    WebDavClient *m_client;
    SyncSettings m_settings;
    QString m_serverPassword;
    QString m_masterPassword;
    std::atomic_bool m_cancelled{false};
    std::atomic_bool m_running{false};
};
