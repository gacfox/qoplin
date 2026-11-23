#pragma once

#include <QDialog>

namespace Ui { class NotebookDialog; }

class NotebookDialog : public QDialog
{
    Q_OBJECT
public:
    explicit NotebookDialog(QWidget *parent = nullptr);
    ~NotebookDialog() override;

    QString notebookName() const;
    QString rootPath() const;
    QString masterPassword() const;
    void accept() override;

private:
    Ui::NotebookDialog *ui;
};
