#include "StartWindow.h"
#include "ui_StartWindow.h"
#include "NotebookDialog.h"
#include "RemoteImportDialog.h"
#include "SettingsDialog.h"
#include "UnlockNotebookDialog.h"
#include "storage/AppSettings.h"
#include "storage/NotebookStore.h"

#include <QDir>
#include <QFileDialog>
#include <QInputDialog>
#include <QListWidgetItem>
#include <QMessageBox>

namespace { constexpr int PathRole = Qt::UserRole; }

StartWindow::StartWindow(AppSettings *settings, QWidget *parent)
    : QMainWindow(parent), ui(new Ui::StartWindow), m_settings(settings)
{
    ui->setupUi(this);
    ui->autoOpenCheck->setChecked(settings->autoOpenLast());
    refreshNotebooks();

    connect(ui->openButton, &QPushButton::clicked, this, [this] {
        if (!selectedPath().isEmpty()) emit openRequested(selectedPath());
    });
    connect(ui->notebookList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) {
        if (!selectedPath().isEmpty()) emit openRequested(selectedPath());
    });
    connect(ui->newButton, &QPushButton::clicked, this, [this] {
        NotebookDialog dialog(this);
        if (dialog.exec() == QDialog::Accepted)
            emit createRequested(dialog.notebookName(), dialog.rootPath(), dialog.masterPassword());
    });
    connect(ui->existingButton, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getExistingDirectory(this, tr("打开已有 Qoplin 笔记本"));
        if (!path.isEmpty()) emit openRequested(QDir::cleanPath(path));
    });
    connect(ui->importButton, &QPushButton::clicked, this, [this] {
        RemoteImportDialog dialog(m_settings, this);
        if (dialog.exec() == QDialog::Accepted && !dialog.importedPath().isEmpty())
            emit importedRequested(dialog.importedPath(), dialog.masterPassword());
    });
    connect(ui->renameButton, &QPushButton::clicked, this, [this] {
        const QString path = selectedPath();
        if (path.isEmpty()) return;
        UnlockNotebookDialog unlock(this);
        unlock.setNotebookName(QDir(path).dirName());
        if (unlock.exec() != QDialog::Accepted) return;
        NotebookStore store;
        QString error;
        if (!store.open(path, unlock.masterPassword(), m_settings->deviceId(), &error)) {
            QMessageBox::critical(this, tr("解锁失败"), error);
            return;
        }
        const QString oldName = store.name();
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("重命名笔记本"), tr("名称："), QLineEdit::Normal, oldName, &ok).trimmed();
        if (!ok || name.isEmpty() || name == oldName) return;
        if (!store.setName(name, &error)) {
            QMessageBox::critical(this, tr("重命名失败"), error);
            return;
        }

        refreshNotebooks();
    });
    connect(ui->removeButton, &QPushButton::clicked, this, [this] {
        const QString path = selectedPath();
        if (path.isEmpty()) return;
        const auto result = QMessageBox::question(this, tr("从列表移除"),
            tr("只会从 Qoplin 列表中移除此笔记本，不会删除任何磁盘文件。\n\n是否继续？"),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (result == QMessageBox::Yes) {
            m_settings->unregisterNotebook(path);
            refreshNotebooks();
        }
    });
    connect(ui->settingsButton, &QPushButton::clicked, this, [this] {
        SettingsDialog dialog(m_settings, this);
        dialog.exec();
        ui->autoOpenCheck->setChecked(m_settings->autoOpenLast());
    });
    connect(ui->autoOpenCheck, &QCheckBox::toggled, this, [this](bool value) {
        m_settings->setAutoOpenLast(value);
    });
    connect(ui->actionOpen, &QAction::triggered, ui->openButton, &QPushButton::click);
    connect(ui->actionNew, &QAction::triggered, ui->newButton, &QPushButton::click);
    connect(ui->actionExisting, &QAction::triggered, ui->existingButton, &QPushButton::click);
    connect(ui->actionImport, &QAction::triggered, ui->importButton, &QPushButton::click);
    connect(ui->actionRenameNotebook, &QAction::triggered, ui->renameButton, &QPushButton::click);
    connect(ui->actionRemoveNotebook, &QAction::triggered, ui->removeButton, &QPushButton::click);
    connect(ui->actionSettings, &QAction::triggered, ui->settingsButton, &QPushButton::click);
    connect(ui->actionExit, &QAction::triggered, this, &QMainWindow::close);
    connect(ui->actionAbout, &QAction::triggered, this, [this] {
        QMessageBox::about(this, tr("关于 Qoplin"),
            tr("<h3>Qoplin 0.1.0</h3><p>轻量、本地优先的 Markdown 笔记。</p>"));
    });
    connect(ui->notebookList, &QListWidget::itemSelectionChanged, this, &StartWindow::updateButtons);
    updateButtons();
}

StartWindow::~StartWindow() { delete ui; }

void StartWindow::refreshNotebooks()
{
    ui->notebookList->clear();
    const auto notebooks = m_settings->notebooks();
    for (const NotebookInfo &info : notebooks) {
        auto *item = new QListWidgetItem(QStringLiteral("%1\n%2").arg(info.name, QDir::toNativeSeparators(info.rootPath)));
        item->setData(PathRole, info.rootPath);
        item->setData(Qt::UserRole + 1, info.name);
        item->setToolTip(info.rootPath);
        ui->notebookList->addItem(item);
    }
    if (ui->notebookList->count() > 0)
        ui->notebookList->setCurrentRow(0);
    updateButtons();
}

QString StartWindow::selectedPath() const
{
    const auto *item = ui->notebookList->currentItem();
    return item ? item->data(PathRole).toString() : QString();
}

void StartWindow::updateButtons()
{
    const bool selected = !selectedPath().isEmpty();
    ui->openButton->setEnabled(selected);
    ui->renameButton->setEnabled(selected);
    ui->removeButton->setEnabled(selected);
    ui->actionOpen->setEnabled(selected);
    ui->actionRenameNotebook->setEnabled(selected);
    ui->actionRemoveNotebook->setEnabled(selected);
}
