#pragma once

#include "models/NoteNode.h"

#include <QHash>
#include <QString>
#include <QVector>

class NotebookStore
{
public:
    NotebookStore() = default;

    bool create(const QString &rootPath, const QString &name, const QString &writerId, QString *error = nullptr);
    bool createWithId(const QString &rootPath, const QString &name, const QString &notebookId,
                      const QString &writerId, QString *error = nullptr);
    bool open(const QString &rootPath, QString *error = nullptr);

    bool isOpen() const { return m_open; }
    QString id() const { return m_id; }
    QString name() const { return m_name; }
    QString rootPath() const { return m_rootPath; }
    QString writerId() const { return m_writerId; }
    void setWriterId(const QString &writerId) { if (!writerId.isEmpty()) m_writerId = writerId; }
    QVector<NoteNode> nodes(bool includeDeleted = false) const;
    NoteNode node(const QString &id) const;
    bool contains(const QString &id) const { return m_nodes.contains(id); }
    QStringList childIds(const QString &parentId, bool includeDeleted = false) const;
    QStringList subtreeIds(const QString &id) const;

    bool setName(const QString &name, QString *error = nullptr);
    QString createNode(NoteNode::Type type, const QString &parentId,
                       const QString &title, QString *error = nullptr);
    bool renameNode(const QString &id, const QString &title, QString *error = nullptr);
    bool moveNode(const QString &id, const QString &newParentId, QString *error = nullptr)
    { return moveNode(id, newParentId, -1, error); }
    bool moveNode(const QString &id, const QString &newParentId, int sortOrder, QString *error = nullptr);
    bool removeNode(const QString &id, QString *error = nullptr);

    QString readNote(const QString &id, QString *error = nullptr) const;
    bool writeNote(const QString &id, const QString &content, QString *error = nullptr);

    // Used by synchronization. These methods preserve remote timestamps and writer IDs.
    bool applyRemoteNode(const NoteNode &node, const QString &content, QString *error = nullptr);
    QString notePath(const NoteNode &node) const;

private:
    bool load(QString *error);
    bool saveIndex(QString *error = nullptr) const;
    bool saveMetadata(QString *error = nullptr) const;
    QString internalPath(const QString &name) const;
    QString validateNewParent(const QString &id, const QString &newParentId) const;
    static bool setError(QString *error, const QString &message);

    bool m_open = false;
    QString m_id;
    QString m_name;
    QString m_rootPath;
    QString m_writerId;
    QHash<QString, NoteNode> m_nodes;
};
