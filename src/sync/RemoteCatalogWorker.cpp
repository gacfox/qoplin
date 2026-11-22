#include "RemoteCatalogWorker.h"

#include "WebDavClient.h"

RemoteCatalogWorker::RemoteCatalogWorker(QObject *parent)
    : QObject(parent)
    , m_client(new WebDavClient(this))
{
}

RemoteCatalogWorker::~RemoteCatalogWorker()
{
    m_serverPassword.fill(QChar::Null);
    m_serverPassword.clear();
}

void RemoteCatalogWorker::configure(const SyncSettings &baseSettings,
                                    const QString &serverPassword)
{
    if (m_running.load())
        return;
    m_settings = baseSettings;
    m_serverPassword = serverPassword;
}

void RemoteCatalogWorker::setRequestTimeout(int milliseconds)
{
    m_client->setTimeout(milliseconds);
}

void RemoteCatalogWorker::cancel()
{
    m_cancelled.store(true);
    m_client->cancel();
}

void RemoteCatalogWorker::start()
{
    if (m_running.exchange(true)) {
        emit finished(false, QStringLiteral("Remote catalog listing is already running."), {});
        return;
    }

    m_cancelled.store(false);
    m_client->resetCancellation();
    m_client->configure(m_settings, m_serverPassword);

    const auto complete = [this](bool success, const QString &message,
                                 const QStringList &notebookIds = {}) {
        m_running.store(false);
        emit finished(success, message, notebookIds);
    };

    QString error;
    if (!m_client->isConfigured(&error)) {
        complete(false, error.isEmpty() ? QStringLiteral("The WebDAV configuration is invalid.") : error);
        return;
    }
    if (m_cancelled.load()) {
        complete(false, QStringLiteral("Remote catalog listing was cancelled."));
        return;
    }

    emit progress(QStringLiteral("Listing remote notebooks."));
    const QStringList notebookIds = m_client->listCollections(QString(), &error);
    if (!error.isEmpty()) {
        complete(false, m_cancelled.load()
                            ? QStringLiteral("Remote catalog listing was cancelled.") : error);
        return;
    }
    if (m_cancelled.load()) {
        complete(false, QStringLiteral("Remote catalog listing was cancelled."));
        return;
    }

    complete(true, QStringLiteral("Found %1 remote notebook(s).").arg(notebookIds.size()), notebookIds);
}
