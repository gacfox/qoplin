#pragma once

#include <QDialog>

namespace Ui { class UnlockNotebookDialog; }

class UnlockNotebookDialog : public QDialog
{
    Q_OBJECT
public:
    explicit UnlockNotebookDialog(QWidget *parent = nullptr);
    ~UnlockNotebookDialog() override;

    void setNotebookName(const QString &name);
    QString masterPassword() const;
    void showAuthenticationError();
    void accept() override;

private:
    Ui::UnlockNotebookDialog *ui;
};
