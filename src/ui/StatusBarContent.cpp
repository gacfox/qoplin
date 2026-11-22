#include "StatusBarContent.h"
#include "ui_StatusBarContent.h"

StatusBarContent::StatusBarContent(QWidget *parent)
    : QWidget(parent), ui(new Ui::StatusBarContent)
{
    ui->setupUi(this);
}

StatusBarContent::~StatusBarContent() { delete ui; }

void StatusBarContent::setSaveText(const QString &text)
{
    ui->statusSaveLabel->setText(text);
}

void StatusBarContent::setSyncText(const QString &text)
{
    ui->statusSyncLabel->setText(text);
}
