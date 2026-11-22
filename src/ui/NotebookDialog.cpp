#include "NotebookDialog.h"
#include "ui_NotebookDialog.h"

#include <QDir>
#include <QFileDialog>
#include <QMessageBox>

NotebookDialog::NotebookDialog(QWidget *parent)
    : QDialog(parent), ui(new Ui::NotebookDialog)
{
    ui->setupUi(this);
    connect(ui->browseButton, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getExistingDirectory(this, tr("选择笔记本位置"), ui->pathEdit->text());
        if (!path.isEmpty())
            ui->pathEdit->setText(QDir::toNativeSeparators(path));
    });
}

NotebookDialog::~NotebookDialog() { delete ui; }

QString NotebookDialog::notebookName() const { return ui->nameEdit->text().trimmed(); }
QString NotebookDialog::rootPath() const { return QDir::cleanPath(ui->pathEdit->text().trimmed()); }

void NotebookDialog::accept()
{
    if (notebookName().isEmpty()) {
        QMessageBox::warning(this, tr("新建笔记本"), tr("请输入笔记本名称。"));
        ui->nameEdit->setFocus();
        return;
    }
    if (rootPath().isEmpty()) {
        QMessageBox::warning(this, tr("新建笔记本"), tr("请选择磁盘位置。"));
        ui->browseButton->setFocus();
        return;
    }
    QDialog::accept();
}
