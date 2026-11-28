#pragma once

#include <QPlainTextEdit>

class QMimeData;

class NotePlainTextEdit : public QPlainTextEdit
{
    Q_OBJECT
public:
    explicit NotePlainTextEdit(QWidget *parent = nullptr);

signals:
    void imagePasteRequested(const QMimeData *mimeData);

protected:
    void insertFromMimeData(const QMimeData *source) override;
};
