#include "MainWindow.h"
#include "ui_MainWindow.h"
#include "SyncDialog.h"
#include "StatusBarContent.h"
#include "storage/AppSettings.h"
#include "storage/NotebookStore.h"
#include "sync/SyncWorker.h"

#include <QMessageBox>
#include <QThread>

void MainWindow::openSyncDialog()
{
    if (m_syncThread) {
        QMessageBox::information(this, tr("同步进行中"), tr("当前同步尚未完成。"));
        return;
    }
    SyncDialog dialog(m_store.get(), m_settings, this);
    connect(&dialog, &SyncDialog::syncRequested, this, &MainWindow::startSync);
    dialog.exec();
}

void MainWindow::synchronizeNow()
{
    if (m_syncThread) {
        QMessageBox::information(this, tr("同步进行中"), tr("当前同步尚未完成。"));
        return;
    }
    const SyncSettings config = m_settings->syncSettings(m_store->id());
    if (!config.enabled || config.endpoint.trimmed().isEmpty() || config.masterPassword.isEmpty()) {
        openSyncDialog();
        return;
    }
    startSync(config.serverPassword, config.masterPassword);
}

void MainWindow::startSync(const QString &serverPassword, const QString &masterPassword)
{
    if (m_syncThread) return;
    const UnsavedAction unsavedAction = resolveUnsavedChanges();
    if (unsavedAction == UnsavedAction::Cancel) return;
    if (unsavedAction == UnsavedAction::Discard) {
        loadNote(m_currentNoteId);
        if (m_dirty) return;
    }
    const SyncSettings config = m_settings->syncSettings(m_store->id());
    if (!config.enabled) return;

    m_syncThread = new QThread(this);
    m_syncWorker = new SyncWorker(m_store.get());
    m_syncWorker->configure(config, serverPassword, masterPassword);
    m_syncWorker->moveToThread(m_syncThread);
    connect(m_syncThread, &QThread::started, m_syncWorker, &SyncWorker::start);
    connect(m_syncWorker, &SyncWorker::progress, this, [this](const QString &message) {
        m_statusContent->setSyncText(message);
    });
    connect(m_syncWorker, &SyncWorker::finished, this, &MainWindow::syncFinished);
    connect(m_syncWorker, &SyncWorker::finished, m_syncThread, &QThread::quit);
    connect(m_syncWorker, &SyncWorker::finished, m_syncWorker, &QObject::deleteLater);
    connect(m_syncThread, &QThread::finished, m_syncThread, &QObject::deleteLater);
    connect(m_syncThread, &QThread::finished, this, [this] {
        m_syncThread = nullptr;
        m_syncWorker = nullptr;
        ui->mainSplitter->setEnabled(true);
        ui->actionBack->setEnabled(true);
        ui->actionSync->setEnabled(true);
        ui->actionSyncSettings->setEnabled(true);
    });

    ui->mainSplitter->setEnabled(false);
    ui->actionBack->setEnabled(false);
    ui->actionSync->setEnabled(false);
    ui->actionSyncSettings->setEnabled(false);
    m_statusContent->setSyncText(tr("正在准备同步…"));
    m_syncThread->start();
}

void MainWindow::syncFinished(bool success, const QString &message, int uploaded, int downloaded)
{
    m_statusContent->setSyncText(success ? QString() : tr("同步失败"));
    if (success) {
        ui->statusBar->showMessage(tr("同步完成：上传 %1 项，下载 %2 项。")
            .arg(uploaded).arg(downloaded), 8000);
    } else {
        QMessageBox::warning(this, tr("同步失败"), message);
    }

    const QString current = m_currentNoteId;
    setDirty(false);
    rebuildTree(current);
    if (!current.isEmpty() && m_store->contains(current) && !m_store->node(current).deleted) {
        loadNote(current);
        m_statusContent->setSaveText(success ? tr("已保存并同步") : tr("已保存"));
    } else if (!current.isEmpty()) {
        clearEditor();
    }
}
