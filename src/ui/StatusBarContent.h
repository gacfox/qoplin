#pragma once

#include <QWidget>

namespace Ui { class StatusBarContent; }

class StatusBarContent : public QWidget
{
public:
    explicit StatusBarContent(QWidget *parent = nullptr);
    ~StatusBarContent() override;

    void setSaveText(const QString &text);
    void setSyncText(const QString &text);

private:
    Ui::StatusBarContent *ui;
};
