#pragma once

#include <QMetaType>
#include <QString>

struct AssetRecord
{
    QString id;
    QString mimeType;
    QString extension;
    QString originalName;
    qint64 size = 0;
    QString sha256;
    qint64 createdAt = 0;
    qint64 updatedAt = 0;
    QString writerId;
    bool deleted = false;
};

Q_DECLARE_METATYPE(AssetRecord)
