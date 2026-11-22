#pragma once

#include <QDialog>

class AppSettings;
namespace Ui { class SettingsDialog; }

class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SettingsDialog(AppSettings *settings, QWidget *parent = nullptr);
    ~SettingsDialog() override;
    void accept() override;

private:
    Ui::SettingsDialog *ui;
    AppSettings *m_settings;
};
