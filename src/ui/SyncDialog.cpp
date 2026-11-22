#include "SyncDialog.h"
#include "ui_SyncDialog.h"
#include "storage/AppSettings.h"
#include "storage/NotebookStore.h"

#include <QAuthenticator>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>

SyncDialog::SyncDialog(NotebookStore *store, AppSettings *settings, QWidget *parent)
    : QDialog(parent), ui(new Ui::SyncDialog), m_store(store), m_settings(settings),
      m_network(new QNetworkAccessManager(this))
{
    ui->setupUi(this);
    const SyncSettings config = settings->syncSettings(store->id());
    ui->enabledCheck->setChecked(config.enabled);
    ui->endpointEdit->setText(config.endpoint);
    ui->remotePathEdit->setText(config.remotePath);
    ui->userEdit->setText(config.userName);
    ui->passwordEdit->setText(config.serverPassword);
    ui->masterPasswordEdit->setText(config.masterPassword);

    connect(ui->closeButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(ui->saveButton, &QPushButton::clicked, this, [this] {
        if (saveSettings()) { ui->messageLabel->setText(tr("配置已保存。密码已以明文保存在本机配置中。")); }
    });
    connect(ui->syncButton, &QPushButton::clicked, this, [this] {
        if (!saveSettings() || !validateForSync()) return;
        emit syncRequested(ui->passwordEdit->text(), ui->masterPasswordEdit->text());
        accept();
    });
    connect(ui->testButton, &QPushButton::clicked, this, &SyncDialog::testConnection);
    connect(m_network, &QNetworkAccessManager::authenticationRequired, this,
        [this](QNetworkReply *, QAuthenticator *authenticator) {
            authenticator->setUser(ui->userEdit->text());
            authenticator->setPassword(ui->passwordEdit->text());
        });
}

SyncDialog::~SyncDialog() { delete ui; }

bool SyncDialog::saveSettings()
{
    SyncSettings config;
    config.enabled = ui->enabledCheck->isChecked();
    config.endpoint = ui->endpointEdit->text().trimmed();
    config.remotePath = ui->remotePathEdit->text().trimmed().remove(QRegularExpression(QStringLiteral("^[/]+|[/]+$")));
    config.userName = ui->userEdit->text().trimmed();
    config.serverPassword = ui->passwordEdit->text();
    config.masterPassword = ui->masterPasswordEdit->text();
    if (config.remotePath.isEmpty()) config.remotePath = QStringLiteral("Qoplin");
    if (config.enabled && config.endpoint.isEmpty()) {
        ui->messageLabel->setText(tr("启用同步前请填写 WebDAV 地址。"));
        return false;
    }
    m_settings->setSyncSettings(m_store->id(), config);
    return true;
}

bool SyncDialog::validateForSync()
{
    if (!ui->enabledCheck->isChecked()) {
        ui->messageLabel->setText(tr("请先勾选“为此笔记本启用 WebDAV 同步”。"));
        return false;
    }
    const QUrl url(ui->endpointEdit->text().trimmed());
    if (!url.isValid() || (url.scheme() != QStringLiteral("https") && url.scheme() != QStringLiteral("http")) || url.host().isEmpty()) {
        ui->messageLabel->setText(tr("请输入有效的 HTTP(S) WebDAV 地址。"));
        return false;
    }
    if (ui->masterPasswordEdit->text().size() < 8) {
        ui->messageLabel->setText(tr("主密码至少需要 8 个字符。忘记主密码将无法恢复远端数据。"));
        return false;
    }
    if (url.scheme() == QStringLiteral("http")) {
        const auto choice = QMessageBox::warning(this, tr("不安全的连接"),
            tr("服务器凭据将通过非加密 HTTP 连接传输。虽然笔记内容仍会端到端加密，仍建议改用 HTTPS。\n\n是否继续？"),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (choice != QMessageBox::Yes) return false;
    }
    return true;
}

void SyncDialog::testConnection()
{
    if (m_testReply) { m_testReply->abort(); m_testReply->deleteLater(); m_testReply = nullptr; }
    const QUrl url(ui->endpointEdit->text().trimmed());
    if (!url.isValid() || url.host().isEmpty()) { ui->messageLabel->setText(tr("请先填写有效的 WebDAV 地址。")); return; }
    QNetworkRequest request(url);
    request.setRawHeader("Depth", "0");
    request.setTransferTimeout(15000);
    m_testReply = m_network->sendCustomRequest(request, "PROPFIND");
    ui->testButton->setEnabled(false);
    ui->messageLabel->setText(tr("正在连接…"));
    connect(m_testReply, &QNetworkReply::finished, this, [this] {
        QNetworkReply *reply = m_testReply;
        m_testReply = nullptr;
        ui->testButton->setEnabled(true);
        if (!reply) return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() == QNetworkReply::NoError || (status >= 200 && status < 400))
            ui->messageLabel->setText(tr("连接成功（HTTP %1）。").arg(status));
        else
            ui->messageLabel->setText(tr("连接失败：%1（HTTP %2）").arg(reply->errorString()).arg(status));
        reply->deleteLater();
    });
}
