#pragma once

#include <QDialog>

class AppSettings;
class QCloseEvent;
class QThread;
namespace Ui { class RemoteImportDialog; }

class RemoteImportDialog : public QDialog
{
    Q_OBJECT
public:
    explicit RemoteImportDialog(AppSettings *settings, QWidget *parent = nullptr);
    ~RemoteImportDialog() override;
    QString importedPath() const { return m_importedPath; }
    QString masterPassword() const;
    void reject() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    bool validateServer();
    void queryRemote();
    void importRemote();
    void cancelOperation();
    void setBusy(bool busy);
    void showMessage(const QString &message);
    void threadFinished();

    Ui::RemoteImportDialog *ui;
    AppSettings *m_settings;
    QThread *m_thread = nullptr;
    QObject *m_worker = nullptr;
    QString m_importedPath;
    bool m_importing = false;
    bool m_importSucceeded = false;
    bool m_closeAfterCancel = false;
};
