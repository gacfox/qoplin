#include "RemoteImportDialog.h"
#include "ui_RemoteImportDialog.h"
#include "storage/AppSettings.h"
#include "storage/NotebookStore.h"
#include "sync/RemoteCatalogWorker.h"
#include "sync/RemoteImportWorker.h"

#include <QCloseEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QRegularExpression>
#include <QThread>
#include <QUuid>

RemoteImportDialog::RemoteImportDialog(AppSettings *settings, QWidget *parent)
    : QDialog(parent), ui(new Ui::RemoteImportDialog), m_settings(settings)
{
    ui->setupUi(this);
    connect(ui->queryButton, &QPushButton::clicked, this, &RemoteImportDialog::queryRemote);
    connect(ui->importButton, &QPushButton::clicked, this, &RemoteImportDialog::importRemote);
    connect(ui->closeButton, &QPushButton::clicked, this, [this] {
        if (m_thread) cancelOperation(); else reject();
    });
    connect(ui->browseButton, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getExistingDirectory(this, tr("选择本地父目录"), ui->directoryEdit->text());
        if (!path.isEmpty()) ui->directoryEdit->setText(QDir::toNativeSeparators(path));
    });
}

RemoteImportDialog::~RemoteImportDialog() { delete ui; }

bool RemoteImportDialog::validateServer()
{
    const QUrl url(ui->endpointEdit->text().trimmed());
    if (!url.isValid() || (url.scheme() != QStringLiteral("https") && url.scheme() != QStringLiteral("http"))
        || url.host().isEmpty()) {
        showMessage(tr("请输入有效的 HTTP(S) WebDAV 地址。"));
        return false;
    }
    return true;
}

void RemoteImportDialog::queryRemote()
{
    if (m_thread || !validateServer()) return;
    SyncSettings config;
    config.enabled = true;
    config.endpoint = ui->endpointEdit->text().trimmed();
    config.remotePath = ui->remotePathEdit->text().trimmed().remove(QRegularExpression(QStringLiteral("^[/]+|[/]+$")));
    config.userName = ui->userEdit->text().trimmed();
    if (config.remotePath.isEmpty()) config.remotePath = QStringLiteral("Qoplin");

    m_importing = false;
    m_importSucceeded = false;
    setBusy(true);
    showMessage(tr("正在查询远端笔记本…"));
    m_thread = new QThread(this);
    auto *worker = new RemoteCatalogWorker;
    worker->configure(config, ui->passwordEdit->text());
    worker->moveToThread(m_thread);
    m_worker = worker;
    connect(m_thread, &QThread::started, worker, &RemoteCatalogWorker::start);
    connect(worker, &RemoteCatalogWorker::progress, this, [this](const QString &message) { showMessage(message); });
    connect(worker, &RemoteCatalogWorker::finished, this,
        [this](bool success, const QString &message, const QStringList &notebookIds) {
            showMessage(message);
            if (success) {
                ui->notebookCombo->clear();
                ui->notebookCombo->addItems(notebookIds);
                if (notebookIds.isEmpty()) showMessage(tr("远端目录中没有可导入的笔记本。"));
            }
        });
    connect(worker, &RemoteCatalogWorker::finished, m_thread, &QThread::quit);
    connect(worker, &RemoteCatalogWorker::finished, worker, &QObject::deleteLater);
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    connect(m_thread, &QThread::finished, this, &RemoteImportDialog::threadFinished);
    m_thread->start();
}

void RemoteImportDialog::importRemote()
{
    if (m_thread || !validateServer()) return;
    const QUuid remoteUuid(ui->notebookCombo->currentText().trimmed());
    if (remoteUuid.isNull()) {
        showMessage(tr("请选择或输入有效的远端笔记本 UUID。"));
        return;
    }
    if (ui->masterPasswordEdit->text().size() < 8) {
        showMessage(tr("主密码至少需要 8 个字符。"));
        return;
    }
    const QString parentPath = QDir::cleanPath(ui->directoryEdit->text().trimmed());
    if (!QFileInfo(parentPath).isDir()) {
        showMessage(tr("请选择有效的本地父目录。"));
        return;
    }
    const QString requestedName = ui->nameEdit->text().trimmed();
    if (requestedName.contains(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*]")))) {
        showMessage(tr("本地名称包含不能用于目录名的字符。"));
        return;
    }
    const QString remoteId = remoteUuid.toString(QUuid::WithoutBraces).toLower();
    const QString folderName = requestedName.isEmpty() ? QStringLiteral("Qoplin-%1").arg(remoteId.left(8)) : requestedName;
    const QString targetPath = QDir(parentPath).filePath(folderName);
    if (QFileInfo::exists(targetPath)) {
        showMessage(tr("目标目录已存在，请更换本地名称或父目录。"));
        return;
    }

    SyncSettings config;
    config.enabled = true;
    config.endpoint = ui->endpointEdit->text().trimmed();
    config.remotePath = ui->remotePathEdit->text().trimmed().remove(QRegularExpression(QStringLiteral("^[/]+|[/]+$")));
    config.userName = ui->userEdit->text().trimmed();
    if (config.remotePath.isEmpty()) config.remotePath = QStringLiteral("Qoplin");

    m_importing = true;
    m_importSucceeded = false;
    m_importedPath.clear();
    setBusy(true);
    showMessage(tr("正在准备导入…"));
    m_thread = new QThread(this);
    auto *worker = new RemoteImportWorker;
    worker->configure(config, ui->passwordEdit->text(), ui->masterPasswordEdit->text(), remoteId,
                      targetPath, requestedName, m_settings->deviceId());
    worker->moveToThread(m_thread);
    m_worker = worker;
    connect(m_thread, &QThread::started, worker, &RemoteImportWorker::start);
    connect(worker, &RemoteImportWorker::progress, this, [this](const QString &message) { showMessage(message); });
    connect(worker, &RemoteImportWorker::finished, this,
        [this, config](bool success, const QString &message, const QString &localRootPath, int importedObjects) {
            showMessage(message);
            if (!success) return;
            m_importedPath = localRootPath;
            m_importSucceeded = true;
            NotebookStore store;
            QString error;
            if (store.open(localRootPath, &error)) {
                SyncSettings saved = config;
                saved.serverPassword = ui->passwordEdit->text();
                saved.masterPassword = ui->masterPasswordEdit->text();
                m_settings->setSyncSettings(store.id(), saved);
            } else {
                m_importSucceeded = false;
                showMessage(tr("导入后无法打开笔记本：%1").arg(error));
                return;
            }
            showMessage(tr("成功导入 %1 个对象。").arg(importedObjects));
        });
    connect(worker, &RemoteImportWorker::finished, m_thread, &QThread::quit);
    connect(worker, &RemoteImportWorker::finished, worker, &QObject::deleteLater);
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    connect(m_thread, &QThread::finished, this, &RemoteImportDialog::threadFinished);
    m_thread->start();
}

void RemoteImportDialog::cancelOperation()
{
    if (!m_thread || !m_worker) return;
    showMessage(tr("正在取消，请稍候…"));
    QMetaObject::invokeMethod(m_worker, "cancel", Qt::DirectConnection);
}

void RemoteImportDialog::showMessage(const QString &message)
{
    ui->messageLabel->setText(message);
    ui->messageLabel->setVisible(!message.isEmpty());
}

void RemoteImportDialog::setBusy(bool busy)
{
    ui->serverGroup->setEnabled(!busy);
    ui->localGroup->setEnabled(!busy);
    ui->queryButton->setEnabled(!busy);
    ui->importButton->setEnabled(!busy);
    ui->progressBar->setVisible(busy);
    ui->closeButton->setText(busy ? tr("取消") : tr("关闭"));
}

void RemoteImportDialog::threadFinished()
{
    m_thread = nullptr;
    m_worker = nullptr;
    setBusy(false);
    if (m_importSucceeded) accept();
    else if (m_closeAfterCancel) reject();
}

void RemoteImportDialog::reject()
{
    if (m_thread) {
        m_closeAfterCancel = true;
        cancelOperation();
        return;
    }
    QDialog::reject();
}

void RemoteImportDialog::closeEvent(QCloseEvent *event)
{
    if (m_thread) {
        m_closeAfterCancel = true;
        cancelOperation();
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}
