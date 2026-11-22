#pragma once

#include "storage/AppSettings.h"

#include <QObject>

class MainWindow;
class StartWindow;

class ApplicationController : public QObject
{
    Q_OBJECT
public:
    explicit ApplicationController(QObject *parent = nullptr);
    void start();

private:
    void showStart();
    void openNotebook(const QString &rootPath);
    void createNotebook(const QString &name, const QString &rootPath);

    AppSettings m_settings;
    StartWindow *m_startWindow = nullptr;
    MainWindow *m_mainWindow = nullptr;
};
