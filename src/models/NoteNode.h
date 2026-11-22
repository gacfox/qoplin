#pragma once

#include <QMetaType>
#include <QString>

struct NoteNode
{
    enum class Type { Folder, Note };

    QString id;
    QString parentId;
    Type type = Type::Note;
    QString title;
    QString fileName;
    qint64 createdAt = 0;
    qint64 updatedAt = 0;
    QString writerId;
    int sortOrder = 0;
    bool deleted = false;

    bool isFolder() const { return type == Type::Folder; }
    bool isNote() const { return type == Type::Note; }
};

Q_DECLARE_METATYPE(NoteNode)
