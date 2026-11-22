#pragma once

#include <QString>

struct NotebookInfo
{
    QString id;
    QString name;
    QString rootPath;
    qint64 lastOpened = 0;
};
