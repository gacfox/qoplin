#pragma once

#include <QDialog>

class AppSettings;
class NotebookStore;
class QNetworkAccessManager;
class QNetworkReply;
namespace Ui { class SyncDialog; }

class SyncDialog : public QDialog
{
    Q_OBJECT
public:
    SyncDialog(NotebookStore *store, AppSettings *settings, QWidget *parent = nullptr);
    ~SyncDialog() override;

signals:
    void syncRequested(const QString &serverPassword, const QString &masterPassword);

private:
    bool saveSettings();
    bool validateForSync();
    void testConnection();

    Ui::SyncDialog *ui;
    NotebookStore *m_store;
    AppSettings *m_settings;
    QNetworkAccessManager *m_network;
    QNetworkReply *m_testReply = nullptr;
};
