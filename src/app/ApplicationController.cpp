#include "ApplicationController.h"
#include "ui/MainWindow.h"
#include "ui/StartWindow.h"
#include "storage/NotebookStore.h"

#include <QDateTime>
#include <QMessageBox>
#include <QTimer>

#include <memory>

ApplicationController::ApplicationController(QObject *parent) : QObject(parent) {}

void ApplicationController::start()
{
    showStart();
    if (m_settings.autoOpenLast()) {
        const QString path = m_settings.lastNotebook();
        if (!path.isEmpty()) QTimer::singleShot(0, this, [this, path] { openNotebook(path); });
    }
}

void ApplicationController::showStart()
{
    if (!m_startWindow) {
        m_startWindow = new StartWindow(&m_settings);
        m_startWindow->setAttribute(Qt::WA_DeleteOnClose);
        connect(m_startWindow, &StartWindow::openRequested, this, &ApplicationController::openNotebook);
        connect(m_startWindow, &StartWindow::createRequested, this, &ApplicationController::createNotebook);
    }
    m_startWindow->refreshNotebooks();
    m_startWindow->show();
    m_startWindow->raise();
}

void ApplicationController::openNotebook(const QString &rootPath)
{
    auto store = std::make_unique<NotebookStore>();
    QString error;
    if (!store->open(rootPath, &error)) {
        QMessageBox::critical(m_startWindow, tr("打开笔记本失败"), error);
        return;
    }
    store->setWriterId(m_settings.deviceId());
    const QString id = store->id();
    const QString name = store->name();
    NotebookInfo info{id, name, rootPath, QDateTime::currentMSecsSinceEpoch()};
    m_settings.registerNotebook(info);
    m_settings.touchNotebook(rootPath);
    m_settings.setLastNotebook(rootPath);

    m_mainWindow = new MainWindow(store.release(), &m_settings);
    m_mainWindow->setAttribute(Qt::WA_DeleteOnClose);
    connect(m_mainWindow, &MainWindow::backRequested, this, [this] {
        if (!m_mainWindow) return;
        m_mainWindow->close();
        m_mainWindow = nullptr;
        showStart();
    });
    connect(m_mainWindow, &QObject::destroyed, this, [this] { m_mainWindow = nullptr; });
    m_startWindow->hide();
    m_mainWindow->show();
}

void ApplicationController::createNotebook(const QString &name, const QString &rootPath)
{
    auto store = std::make_unique<NotebookStore>();
    QString error;
    if (!store->create(rootPath, name, m_settings.deviceId(), &error)) {
        QMessageBox::critical(m_startWindow, tr("新建笔记本失败"), error);
        return;
    }
    const QString id = store->id();
    const QString actualName = store->name();
    NotebookInfo info{id, actualName, rootPath, QDateTime::currentMSecsSinceEpoch()};
    m_settings.registerNotebook(info);
    m_settings.setLastNotebook(rootPath);
    m_mainWindow = new MainWindow(store.release(), &m_settings);
    m_mainWindow->setAttribute(Qt::WA_DeleteOnClose);
    connect(m_mainWindow, &MainWindow::backRequested, this, [this] {
        if (!m_mainWindow) return;
        m_mainWindow->close();
        m_mainWindow = nullptr;
        showStart();
    });
    connect(m_mainWindow, &QObject::destroyed, this, [this] { m_mainWindow = nullptr; });
    m_startWindow->hide();
    m_mainWindow->show();
}
