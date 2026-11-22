#pragma once

#include <QMainWindow>

class AppSettings;
namespace Ui { class StartWindow; }

class StartWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit StartWindow(AppSettings *settings, QWidget *parent = nullptr);
    ~StartWindow() override;
    void refreshNotebooks();

signals:
    void openRequested(const QString &rootPath);
    void createRequested(const QString &name, const QString &rootPath);

private:
    QString selectedPath() const;
    void updateButtons();

    Ui::StartWindow *ui;
    AppSettings *m_settings;
};
