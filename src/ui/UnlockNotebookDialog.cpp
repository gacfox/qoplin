#include "UnlockNotebookDialog.h"
#include "ui_UnlockNotebookDialog.h"

#include <QMessageBox>

UnlockNotebookDialog::UnlockNotebookDialog(QWidget *parent)
    : QDialog(parent), ui(new Ui::UnlockNotebookDialog)
{
    ui->setupUi(this);
}

UnlockNotebookDialog::~UnlockNotebookDialog() { delete ui; }

void UnlockNotebookDialog::setNotebookName(const QString &name)
{
    ui->promptLabel->setText(tr("请输入笔记本“%1”的主密码。").arg(name));
}

QString UnlockNotebookDialog::masterPassword() const { return ui->passwordEdit->text(); }

void UnlockNotebookDialog::showAuthenticationError()
{
    ui->messageLabel->setText(tr("主密码错误或数据已损坏。"));
    ui->messageLabel->setVisible(true);
    ui->passwordEdit->selectAll();
    ui->passwordEdit->setFocus();
}

void UnlockNotebookDialog::accept()
{
    if (ui->passwordEdit->text().isEmpty()) {
        QMessageBox::warning(this, tr("解锁笔记本"), tr("请输入主密码。"));
        return;
    }
    QDialog::accept();
}
