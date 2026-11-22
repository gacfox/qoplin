#include "SettingsDialog.h"
#include "ui_SettingsDialog.h"
#include "storage/AppSettings.h"

SettingsDialog::SettingsDialog(AppSettings *settings, QWidget *parent)
    : QDialog(parent), ui(new Ui::SettingsDialog), m_settings(settings)
{
    ui->setupUi(this);
    ui->autoOpenCheck->setChecked(settings->autoOpenLast());
    ui->modeCombo->setCurrentIndex(settings->editorMode());
    ui->fontCombo->setCurrentFont(QFont(settings->editorFontFamily()));
    ui->fontSizeSpin->setValue(settings->editorFontSize());
}

SettingsDialog::~SettingsDialog() { delete ui; }

void SettingsDialog::accept()
{
    m_settings->setAutoOpenLast(ui->autoOpenCheck->isChecked());
    m_settings->setEditorMode(ui->modeCombo->currentIndex());
    m_settings->setEditorFontFamily(ui->fontCombo->currentFont().family());
    m_settings->setEditorFontSize(ui->fontSizeSpin->value());
    QDialog::accept();
}
