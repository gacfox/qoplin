#pragma once

#include "models/NoteNode.h"

#include <QHash>
#include <QString>
#include <QVector>

#include <memory>

class CryptoService;

// An encrypted local notebook.
//
// qoplin.json is a non-secret crypto header (format 'qoplin-encrypted-local', version 1,
// notebook id, KDF salt/ops/mem, key check). It never contains the notebook name, any
// node data, the writer ID, or key material. Everything else on disk is AEAD ciphertext:
// .qoplin/index.bin holds {version, notebookName, nodes} and .qoplin/notes/<uuid>.bin
// holds the UTF-8 markdown of a note. Old plaintext notebooks are not supported.
class NotebookStore
{
public:
    NotebookStore();
    ~NotebookStore();

    NotebookStore(const NotebookStore &) = delete;
    NotebookStore &operator=(const NotebookStore &) = delete;

    bool create(const QString &rootPath, const QString &name, const QString &writerId,
                const QString &masterPassword, QString *error = nullptr);
    bool createWithId(const QString &rootPath, const QString &name, const QString &notebookId,
                      const QString &writerId, const QString &masterPassword, QString *error = nullptr);
    bool open(const QString &rootPath, const QString &masterPassword, const QString &writerId,
              QString *error = nullptr);
    // Closes the notebook and clears the derived key and the master password.
    void close();

    bool isOpen() const { return m_open; }
    QString id() const { return m_id; }
    QString name() const { return m_name; }
    QString rootPath() const { return m_rootPath; }
    QString writerId() const { return m_writerId; }
    void setWriterId(const QString &writerId) { if (!writerId.isEmpty()) m_writerId = writerId; }
    // Held in memory only while the notebook is open; never written to disk.
    QString masterPassword() const { return m_masterPassword; }
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
    QString internalPath(const QString &name) const;
    QString noteFilePath(const QString &noteId) const;
    bool writeNoteFile(const QString &noteId, const QByteArray &plainText, QString *error) const;
    // Reads, authenticates, and parses an encrypted JSON object. The verified raw
    // ciphertext is returned as well, so a recovery can rewrite it without re-encrypting.
    bool readEncryptedJson(const QString &path, const QByteArray &associatedData,
                           QByteArray *cipherText, QJsonObject *object, QString *error) const;
    QString validateNewParent(const QString &id, const QString &newParentId) const;
    static bool setError(QString *error, const QString &message);

    bool m_open = false;
    QString m_id;
    QString m_name;
    QString m_rootPath;
    QString m_writerId;
    QString m_masterPassword;
    QHash<QString, NoteNode> m_nodes;
    // Holds the derived key; destroyed by close() and the destructor.
    std::unique_ptr<CryptoService> m_crypto;
};
