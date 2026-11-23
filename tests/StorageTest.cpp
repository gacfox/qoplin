#include "StorageTest.h"
#include "storage/AppSettings.h"
#include "storage/NotebookStore.h"
#include "sync/CryptoService.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

namespace
{
const QString kPassword = QStringLiteral("correct horse battery staple 主密码");
const QString kWriter = QStringLiteral("device-a");
const QString kNotebookName = QStringLiteral("Secret Notebook 名称");
const QString kFolderTitle = QStringLiteral("Secret Folder 目录");
const QString kNoteTitle = QStringLiteral("Secret Title 标题");
const QString kNoteContent = QStringLiteral("\x23 Secret Body\n\n正文内容 secret body text.\n");

const QString kHeaderName = QStringLiteral("qoplin.json");
const QString kIndexName = QStringLiteral(".qoplin/index.bin");

QByteArray readFileBytes(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.readAll();
}

bool writeFileBytes(const QString &path, const QByteArray &data)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(data) == data.size();
}

// Every file below root, as a path relative to root with '/' separators.
QStringList notebookFiles(const QString &root)
{
    QStringList files;
    QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        files.append(QDir(root).relativeFilePath(it.filePath()));
    }
    files.sort();
    return files;
}

// Every value that must never be readable inside any file of a notebook.
QStringList secrets()
{
    return {kPassword, kWriter, kNotebookName, kFolderTitle, kNoteTitle, kNoteContent};
}

bool containsSecret(const QByteArray &data, const QString &secret)
{
    if (data.contains(secret.toUtf8()))
        return true;
    return data.contains(QByteArray(reinterpret_cast<const char *>(secret.utf16()), secret.size() * 2));
}

// Creates a notebook with one folder and one written note.
bool populate(NotebookStore *store, const QString &root, QString *noteId, QString *error)
{
    if (!store->create(root, kNotebookName, kWriter, kPassword, error))
        return false;
    const QString folder = store->createNode(NoteNode::Type::Folder, QString(), kFolderTitle, error);
    if (folder.isEmpty())
        return false;
    *noteId = store->createNode(NoteNode::Type::Note, folder, kNoteTitle, error);
    if (noteId->isEmpty())
        return false;
    return store->writeNote(*noteId, kNoteContent, error);
}

QJsonObject readJsonObject(const QString &path)
{
    return QJsonDocument::fromJson(readFileBytes(path)).object();
}

QByteArray flipLastByte(QByteArray data)
{
    data[data.size() - 1] = static_cast<char>(data.at(data.size() - 1) ^ 0x01);
    return data;
}
} // namespace

void StorageTest::notebookLifecycle()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    NotebookStore store;
    QString error;
    QVERIFY2(store.create(temp.path(), QStringLiteral("测试笔记"), QStringLiteral("device-a"), kPassword, &error), qPrintable(error));
    QVERIFY(store.isOpen());
    QCOMPARE(store.masterPassword(), kPassword);
    const QString noteId = store.createNode(NoteNode::Type::Note, QString(), QStringLiteral("第一篇"), &error);
    QVERIFY2(!noteId.isEmpty(), qPrintable(error));
    QCOMPARE(store.node(noteId).fileName, noteId + QStringLiteral(".bin"));
    QVERIFY(store.writeNote(noteId, QStringLiteral("\x23 标题\n\n你好，Qoplin。\n"), &error));
    const QString path = store.notePath(store.node(noteId));
    QVERIFY(QFileInfo::exists(path));
    QCOMPARE(QFileInfo(path).fileName(), noteId + QStringLiteral(".bin"));
    QVERIFY(store.renameNode(noteId, QStringLiteral("新标题"), &error));
    QCOMPARE(store.notePath(store.node(noteId)), path);

    NotebookStore reopened;
    QVERIFY2(reopened.open(temp.path(), kPassword, QStringLiteral("device-b"), &error), qPrintable(error));
    QCOMPARE(reopened.id(), store.id());
    QCOMPARE(reopened.name(), QStringLiteral("测试笔记"));
    QCOMPARE(reopened.writerId(), QStringLiteral("device-b"));
    QCOMPARE(reopened.masterPassword(), kPassword);
    QCOMPARE(reopened.node(noteId).title, QStringLiteral("新标题"));
    QCOMPARE(reopened.node(noteId).fileName, noteId + QStringLiteral(".bin"));
    QCOMPARE(reopened.readNote(noteId, &error), QStringLiteral("\x23 标题\n\n你好，Qoplin。\n"));
    QVERIFY(error.isEmpty());
    reopened.setWriterId(QStringLiteral("device-c"));
    QCOMPARE(reopened.writerId(), QStringLiteral("device-c"));
}

void StorageTest::hierarchyAndTombstones()
{
    QTemporaryDir temp;
    NotebookStore store;
    QString error;
    QVERIFY(store.create(temp.path(), QStringLiteral("Tree"), QStringLiteral("device-a"), kPassword, &error));
    const QString parent = store.createNode(NoteNode::Type::Folder, QString(), QStringLiteral("Parent"), &error);
    const QString child = store.createNode(NoteNode::Type::Folder, parent, QStringLiteral("Child"), &error);
    const QString note = store.createNode(NoteNode::Type::Note, child, QStringLiteral("Note"), &error);
    QVERIFY(!parent.isEmpty() && !child.isEmpty() && !note.isEmpty());
    QCOMPARE(store.node(note).parentId, child);
    QVERIFY(!store.moveNode(parent, child, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(store.moveNode(note, parent, &error));
    QCOMPARE(store.node(note).parentId, parent);
    QVERIFY(store.moveNode(note, child, &error));
    QVERIFY(store.removeNode(parent, &error));
    QVERIFY(store.node(parent).deleted);
    QVERIFY(store.node(child).deleted);
    QVERIFY(store.node(note).deleted);
    QCOMPARE(store.nodes().size(), 0);
    QCOMPARE(store.nodes(true).size(), 3);

    // Tombstones survive a reopen with the master password.
    NotebookStore reopened;
    QVERIFY2(reopened.open(temp.path(), kPassword, QStringLiteral("device-b"), &error), qPrintable(error));
    QCOMPARE(reopened.nodes().size(), 0);
    QCOMPARE(reopened.nodes(true).size(), 3);
    QVERIFY(reopened.node(note).deleted);
    QVERIFY(reopened.readNote(note, &error).isNull());
    QVERIFY(!error.isEmpty());
}

void StorageTest::remoteNodePreservesRevision()
{
    QTemporaryDir temp;
    NotebookStore store;
    QString error;
    QVERIFY(store.create(temp.path(), QStringLiteral("Remote"), QStringLiteral("device-a"), kPassword, &error));
    NoteNode remote;
    remote.id = QStringLiteral("01234567-89ab-cdef-8123-456789abcdef");
    remote.type = NoteNode::Type::Note;
    remote.title = QStringLiteral("Remote note");
    remote.createdAt = 100;
    remote.updatedAt = 123456789;
    remote.writerId = QStringLiteral("device-b");
    QVERIFY2(store.applyRemoteNode(remote, QStringLiteral("remote body"), &error), qPrintable(error));
    const NoteNode saved = store.node(remote.id);
    QCOMPARE(saved.updatedAt, remote.updatedAt);
    QCOMPARE(saved.writerId, remote.writerId);
    QCOMPARE(saved.fileName, remote.id + QStringLiteral(".bin"));
    QCOMPARE(store.readNote(remote.id, &error), QStringLiteral("remote body"));

    // The remote content is stored encrypted and survives a reopen.
    QVERIFY(!containsSecret(readFileBytes(store.notePath(saved)), QStringLiteral("remote body")));
    NotebookStore reopened;
    QVERIFY2(reopened.open(temp.path(), kPassword, QStringLiteral("device-a"), &error), qPrintable(error));
    QCOMPARE(reopened.readNote(remote.id, &error), QStringLiteral("remote body"));
    QCOMPARE(reopened.node(remote.id).updatedAt, remote.updatedAt);

    // A node that still names a plaintext ".md" object belongs to the old format and is rejected.
    NoteNode legacy = remote;
    legacy.id = QStringLiteral("01234567-89ab-cdef-8123-456789abcdee");
    legacy.fileName = legacy.id + QStringLiteral(".md");
    QVERIFY(!reopened.applyRemoteNode(legacy, QStringLiteral("legacy"), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!reopened.contains(legacy.id));
    QVERIFY(!QFileInfo::exists(temp.path() + QStringLiteral("/.qoplin/notes/") + legacy.id + QStringLiteral(".md")));
}

void StorageTest::indexBackupRecovery()
{
    QTemporaryDir temp;
    NotebookStore store;
    QString error;
    QVERIFY(store.create(temp.path(), QStringLiteral("Recovery"), QStringLiteral("device-a"), kPassword, &error));
    const QString note = store.createNode(NoteNode::Type::Note, QString(), QStringLiteral("Recover me"), &error);
    QVERIFY(!note.isEmpty());
    const QString indexPath = temp.path() + QStringLiteral("/.qoplin/index.bin");
    QVERIFY(QFileInfo::exists(indexPath + QStringLiteral(".bak")));
    const QByteArray backup = readFileBytes(indexPath + QStringLiteral(".bak"));
    {
        QFile file(indexPath);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("not json");
    }
    NotebookStore recovered;
    QVERIFY2(recovered.open(temp.path(), kPassword, QStringLiteral("device-a"), &error), qPrintable(error));
    QVERIFY(recovered.contains(note));
    QCOMPARE(recovered.node(note).title, QStringLiteral("Recover me"));
    // The damaged primary copy is repaired from the verified backup ciphertext.
    QCOMPARE(readFileBytes(indexPath), backup);
}

void StorageTest::headerBackupRecovery()
{
    QTemporaryDir temp;
    NotebookStore store;
    QString error;
    QString noteId;
    QVERIFY2(populate(&store, temp.path(), &noteId, &error), qPrintable(error));
    const QString headerPath = temp.path() + QStringLiteral("/") + kHeaderName;
    const QByteArray backup = readFileBytes(headerPath + QStringLiteral(".bak"));
    QVERIFY(writeFileBytes(headerPath, QByteArrayLiteral("not json")));

    NotebookStore recovered;
    QVERIFY2(recovered.open(temp.path(), kPassword, QStringLiteral("device-a"), &error), qPrintable(error));
    QCOMPARE(recovered.id(), store.id());
    QCOMPARE(recovered.name(), kNotebookName);
    QCOMPARE(recovered.readNote(noteId, &error), kNoteContent);
    QCOMPARE(readFileBytes(headerPath), backup);

    // Without the backup the notebook cannot be opened, and nothing is rewritten.
    QVERIFY(QFile::remove(headerPath));
    QVERIFY(QFile::remove(headerPath + QStringLiteral(".bak")));
    QVERIFY(!recovered.open(temp.path(), kPassword, QStringLiteral("device-a"), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!recovered.isOpen());
    QVERIFY(!QFileInfo::exists(headerPath));
}

void StorageTest::syncSettingsRoundTrip()
{
    QCoreApplication::setOrganizationName(QStringLiteral("QoplinTests"));
    QCoreApplication::setApplicationName(QStringLiteral("StorageTest"));
    AppSettings settings;
    SyncSettings expected;
    expected.enabled = true;
    expected.endpoint = QStringLiteral("https://example.test/dav");
    expected.remotePath = QStringLiteral("Notes");
    expected.userName = QStringLiteral("user");
    expected.serverPassword = QStringLiteral("server password");
    const QString notebookId = QStringLiteral("01234567-89ab-cdef-8123-456789abcdef");
    settings.setSyncSettings(notebookId, expected);
    const SyncSettings actual = settings.syncSettings(notebookId);
    QCOMPARE(actual.enabled, expected.enabled);
    QCOMPARE(actual.endpoint, expected.endpoint);
    QCOMPARE(actual.remotePath, expected.remotePath);
    QCOMPARE(actual.userName, expected.userName);
    QCOMPARE(actual.serverPassword, expected.serverPassword);
}

void StorageTest::createWithSpecifiedId()
{
    QTemporaryDir temp;
    NotebookStore store;
    QString error;
    const QString id = QStringLiteral("01234567-89ab-cdef-8123-456789abcdef");
    QVERIFY2(store.createWithId(temp.path(), QStringLiteral("Imported"), id, QStringLiteral("device-a"), kPassword, &error), qPrintable(error));
    QCOMPARE(store.id(), id);
    QCOMPARE(readJsonObject(temp.path() + QStringLiteral("/") + kHeaderName)
                 .value(QStringLiteral("id")).toString(), id);
    NotebookStore reopened;
    QVERIFY2(reopened.open(temp.path(), kPassword, QStringLiteral("device-a"), &error), qPrintable(error));
    QCOMPARE(reopened.id(), id);
    QCOMPARE(reopened.name(), QStringLiteral("Imported"));

    // The ID is normalized like every other UUID.
    NotebookStore upperCase;
    QVERIFY2(upperCase.createWithId(temp.path() + QStringLiteral("/upper"), QStringLiteral("Upper"),
                                    QStringLiteral("{01234567-89AB-CDEF-8123-456789ABCDEF}"), kWriter,
                                    kPassword, &error), qPrintable(error));
    QCOMPARE(upperCase.id(), id);
    NotebookStore invalid;
    QVERIFY(!invalid.createWithId(temp.path() + QStringLiteral("/invalid"), QStringLiteral("Invalid"),
                                  QStringLiteral("not a uuid"), kWriter, kPassword, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!invalid.isOpen());
    QVERIFY(!QFileInfo::exists(temp.path() + QStringLiteral("/invalid")));
}

void StorageTest::headerIsNonSecret()
{
    QTemporaryDir temp;
    NotebookStore store;
    QString noteId;
    QString error;
    QVERIFY2(populate(&store, temp.path(), &noteId, &error), qPrintable(error));

    const QString headerPath = temp.path() + QStringLiteral("/") + kHeaderName;
    const QByteArray headerData = readFileBytes(headerPath);
    const QJsonObject header = readJsonObject(headerPath);
    QCOMPARE(header.value(QStringLiteral("format")).toString(), QStringLiteral("qoplin-encrypted-local"));
    QCOMPARE(CryptoService::headerFormat(true), QStringLiteral("qoplin-encrypted-local"));
    QCOMPARE(header.value(QStringLiteral("version")).toInt(), 1);
    QCOMPARE(header.value(QStringLiteral("id")).toString(), store.id());
    QCOMPARE(header.value(QStringLiteral("kdf")).toString(), QStringLiteral("Argon2id"));
    QCOMPARE(QByteArray::fromBase64(header.value(QStringLiteral("salt")).toString().toLatin1()).size(), 16);
    QCOMPARE(header.value(QStringLiteral("opsLimit")).toString().toULongLong(), CryptoService::defaultOpsLimit());
    QCOMPARE(header.value(QStringLiteral("memLimit")).toString().toULongLong(), CryptoService::defaultMemLimit());
    const QJsonObject keyCheck = header.value(QStringLiteral("keyCheck")).toObject();
    QCOMPARE(QByteArray::fromBase64(keyCheck.value(QStringLiteral("nonce")).toString().toLatin1()).size(), 24);
    QVERIFY(QByteArray::fromBase64(keyCheck.value(QStringLiteral("ciphertext")).toString().toLatin1()).size() >= 16);

    // Exactly the non-secret fields and nothing else.
    QStringList keys = header.keys();
    keys.sort();
    QStringList expectedKeys{QStringLiteral("format"), QStringLiteral("id"), QStringLiteral("kdf"),
                             QStringLiteral("keyCheck"), QStringLiteral("memLimit"),
                             QStringLiteral("opsLimit"), QStringLiteral("salt"), QStringLiteral("version")};
    expectedKeys.sort();
    QCOMPARE(keys, expectedKeys);

    // The backup is an identical plaintext copy of the same non-secret header.
    QCOMPARE(readFileBytes(headerPath + QStringLiteral(".bak")), headerData);
    for (const QString &secret : secrets())
        QVERIFY2(!containsSecret(headerData, secret), qPrintable(QStringLiteral("Header leaks %1").arg(secret)));
}

void StorageTest::onDiskFilesAreEncrypted()
{
    QTemporaryDir temp;
    NotebookStore store;
    QString noteId;
    QString error;
    QVERIFY2(populate(&store, temp.path(), &noteId, &error), qPrintable(error));
    const QString root = temp.path();

    // Only the header exists in plaintext; no Markdown or JSON index is left behind.
    QStringList expected{kHeaderName, kHeaderName + QStringLiteral(".bak"), kIndexName,
                         kIndexName + QStringLiteral(".bak"),
                         QStringLiteral(".qoplin/notes/") + noteId + QStringLiteral(".bin")};
    expected.sort();
    QCOMPARE(notebookFiles(root), expected);

    for (const QString &file : notebookFiles(root)) {
        const QByteArray data = readFileBytes(root + QLatin1Char('/') + file);
        QVERIFY(!data.isEmpty());
        for (const QString &secret : secrets()) {
            QVERIFY2(!containsSecret(data, secret),
                     qPrintable(QStringLiteral("%1 leaks %2").arg(file, secret)));
        }
        if (!file.startsWith(kHeaderName)) {
            // Ciphertext must never parse as a document.
            QVERIFY2(QJsonDocument::fromJson(data).isNull(), qPrintable(file));
            QVERIFY(!QString::fromUtf8(data).contains(QStringLiteral("secret")));
        }
    }

    // The ciphertext follows the documented format when opened with the header and
    // the documented associated data.
    CryptoService crypto;
    const QJsonObject header = readJsonObject(root + QStringLiteral("/") + kHeaderName);
    QVERIFY2(crypto.initializeFromHeader(header, kPassword, &error, true), qPrintable(error));
    const QByteArray indexCipherText = readFileBytes(root + QStringLiteral("/") + kIndexName);
    QCOMPARE(indexCipherText, readFileBytes(root + QStringLiteral("/") + kIndexName + QStringLiteral(".bak")));

    const QByteArray indexData = QByteArrayLiteral("qoplin/local/index/v1:") + store.id().toUtf8();
    QByteArray plainText;
    QVERIFY2(crypto.decrypt(indexCipherText, indexData, &plainText, &error), qPrintable(error));
    const QJsonObject index = QJsonDocument::fromJson(plainText).object();
    QCOMPARE(index.value(QStringLiteral("version")).toInt(), 1);
    QCOMPARE(index.value(QStringLiteral("notebookName")).toString(), kNotebookName);
    const QJsonArray nodes = index.value(QStringLiteral("nodes")).toArray();
    QCOMPARE(nodes.size(), 2);
    QStringList ids;
    for (const QJsonValue &value : nodes) {
        const QJsonObject object = value.toObject();
        ids.append(object.value(QStringLiteral("id")).toString());
        QStringList nodeKeys = object.keys();
        nodeKeys.sort();
        QStringList expectedNodeKeys{QStringLiteral("createdAt"), QStringLiteral("deleted"),
                                     QStringLiteral("fileName"), QStringLiteral("id"),
                                     QStringLiteral("parentId"), QStringLiteral("sortOrder"),
                                     QStringLiteral("title"), QStringLiteral("type"),
                                     QStringLiteral("updatedAt"), QStringLiteral("writerId")};
        expectedNodeKeys.sort();
        QCOMPARE(nodeKeys, expectedNodeKeys);
        if (object.value(QStringLiteral("type")).toString() == QStringLiteral("note"))
            QCOMPARE(object.value(QStringLiteral("fileName")).toString(), noteId + QStringLiteral(".bin"));
        else
            QVERIFY(object.value(QStringLiteral("fileName")).toString().isEmpty());
    }
    ids.sort();
    QStringList expectedIds{noteId, store.node(noteId).parentId};
    expectedIds.sort();
    QCOMPARE(ids, expectedIds);

    const QByteArray noteData = QByteArrayLiteral("qoplin/local/note/v1:") + store.id().toUtf8()
                                + ':' + noteId.toUtf8();
    QVERIFY2(crypto.decrypt(readFileBytes(store.notePath(store.node(noteId))), noteData, &plainText, &error),
             qPrintable(error));
    QCOMPARE(plainText, kNoteContent.toUtf8());

    // The associated data is enforced: another notebook, another object, or the remote
    // format cannot open a local object.
    QByteArray other;
    QVERIFY(!crypto.decrypt(indexCipherText, QByteArrayLiteral("qoplin/local/index/v1:") + noteId.toUtf8(), &other, &error));
    QVERIFY(!crypto.decrypt(indexCipherText, noteData, &other, &error));
    QVERIFY(!crypto.decrypt(indexCipherText, QByteArrayLiteral("qoplin/manifest/v1"), &other, &error));
    QVERIFY(!crypto.decrypt(readFileBytes(store.notePath(store.node(noteId))), indexData, &other, &error));
    QVERIFY(!crypto.decrypt(readFileBytes(store.notePath(store.node(noteId))), noteData + 'x', &other, &error));
}

void StorageTest::freshNoncePerEncryption()
{
    QTemporaryDir temp;
    NotebookStore store;
    QString noteId;
    QString error;
    QVERIFY2(populate(&store, temp.path(), &noteId, &error), qPrintable(error));
    const QString noteFile = store.notePath(store.node(noteId));
    const QString indexFile = temp.path() + QStringLiteral("/") + kIndexName;

    // Writing identical plaintext again must never reproduce the previous ciphertext.
    const QByteArray firstNote = readFileBytes(noteFile);
    const QByteArray firstIndex = readFileBytes(indexFile);
    QVERIFY(store.writeNote(noteId, kNoteContent, &error));
    const QByteArray secondNote = readFileBytes(noteFile);
    const QByteArray secondIndex = readFileBytes(indexFile);
    QCOMPARE(secondNote.size(), firstNote.size());
    QVERIFY(secondNote != firstNote);
    QVERIFY(secondIndex != firstIndex);
    QCOMPARE(readFileBytes(indexFile + QStringLiteral(".bak")), secondIndex);

    // Two notes with the same (empty) plaintext never share a ciphertext either.
    const QString emptyA = store.createNode(NoteNode::Type::Note, QString(), QStringLiteral("Empty A"), &error);
    const QString emptyB = store.createNode(NoteNode::Type::Note, QString(), QStringLiteral("Empty B"), &error);
    QVERIFY(!emptyA.isEmpty() && !emptyB.isEmpty());
    const QByteArray cipherA = readFileBytes(store.notePath(store.node(emptyA)));
    const QByteArray cipherB = readFileBytes(store.notePath(store.node(emptyB)));
    QCOMPARE(cipherA.size(), 40); // 24 nonce bytes + 16 tag bytes, no plaintext
    QCOMPARE(cipherB.size(), 40);
    QVERIFY(cipherA != cipherB);
    QCOMPARE(store.readNote(emptyA, &error), QString());

    // Two notebooks created with the same password derive different keys.
    NotebookStore twin;
    QVERIFY2(twin.create(temp.path() + QStringLiteral("/twin"), kNotebookName, kWriter, kPassword, &error),
             qPrintable(error));
    QVERIFY(twin.id() != store.id());
    QVERIFY(readJsonObject(temp.path() + QStringLiteral("/twin/") + kHeaderName)
                .value(QStringLiteral("salt")).toString()
            != readJsonObject(temp.path() + QStringLiteral("/") + kHeaderName)
                .value(QStringLiteral("salt")).toString());
    QVERIFY(readFileBytes(temp.path() + QStringLiteral("/twin/") + kIndexName) != secondIndex);
}

void StorageTest::wrongPasswordLeavesNoPartialState()
{
    QTemporaryDir temp;
    NotebookStore store;
    QString noteId;
    QString error;
    QVERIFY2(populate(&store, temp.path(), &noteId, &error), qPrintable(error));
    const QStringList filesBefore = notebookFiles(temp.path());
    const QByteArray indexBefore = readFileBytes(temp.path() + QStringLiteral("/") + kIndexName);

    NotebookStore wrong;
    QVERIFY(!wrong.open(temp.path(), QStringLiteral("not the password"), QStringLiteral("device-a"), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!wrong.isOpen());
    QVERIFY(wrong.id().isEmpty());
    QVERIFY(wrong.name().isEmpty());
    QVERIFY(wrong.rootPath().isEmpty());
    QVERIFY(wrong.writerId().isEmpty());
    QVERIFY(wrong.masterPassword().isEmpty());
    QVERIFY(wrong.nodes(true).isEmpty());
    QVERIFY(!wrong.contains(noteId));
    QVERIFY(!wrong.writeNote(noteId, QStringLiteral("x"), &error));
    QVERIFY(wrong.readNote(noteId, &error).isNull());
    QVERIFY(!wrong.setName(QStringLiteral("x"), &error));
    QVERIFY(wrong.createNode(NoteNode::Type::Note, QString(), QStringLiteral("x"), &error).isEmpty());

    // An empty password is rejected before any key is derived.
    QVERIFY(!wrong.open(temp.path(), QString(), QStringLiteral("device-a"), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!wrong.isOpen());

    // A store that was open is fully reset by a failed re-open.
    QVERIFY(store.isOpen());
    QVERIFY(!store.open(temp.path(), QStringLiteral("still wrong"), QStringLiteral("device-a"), &error));
    QVERIFY(!store.isOpen());
    QVERIFY(store.id().isEmpty());
    QVERIFY(store.name().isEmpty());
    QVERIFY(store.masterPassword().isEmpty());
    QVERIFY(store.nodes(true).isEmpty());

    // Nothing on disk was touched, so the right password still opens the intact notebook.
    QCOMPARE(notebookFiles(temp.path()), filesBefore);
    QCOMPARE(readFileBytes(temp.path() + QStringLiteral("/") + kIndexName), indexBefore);
    QVERIFY2(store.open(temp.path(), kPassword, QStringLiteral("device-a"), &error), qPrintable(error));
    QVERIFY(store.isOpen());
    QVERIFY(store.contains(noteId));
    QCOMPARE(store.readNote(noteId, &error), kNoteContent);

    // A directory without a notebook header is not opened either.
    QVERIFY(QDir(temp.path()).mkpath(QStringLiteral("/empty")));
    QVERIFY(!store.open(temp.path() + QStringLiteral("/empty"), kPassword, QStringLiteral("device-a"), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!store.isOpen());
}

void StorageTest::tamperedFilesAreRejected()
{
    QTemporaryDir temp;
    NotebookStore store;
    QString noteId;
    QString error;
    QVERIFY2(populate(&store, temp.path(), &noteId, &error), qPrintable(error));
    const QString noteFile = store.notePath(store.node(noteId));
    const QString indexFile = temp.path() + QStringLiteral("/") + kIndexName;
    const QByteArray noteBackup = readFileBytes(noteFile);
    const QByteArray indexBackup = readFileBytes(indexFile + QStringLiteral(".bak"));

    // A modified note fails authentication instead of returning damaged text.
    QVERIFY(writeFileBytes(noteFile, flipLastByte(readFileBytes(noteFile))));
    QVERIFY(store.readNote(noteId, &error).isNull());
    QVERIFY(!error.isEmpty());
    QVERIFY(writeFileBytes(noteFile, noteBackup));
    QCOMPARE(store.readNote(noteId, &error), kNoteContent);

    // A damaged primary index falls back to the intact backup and is repaired.
    QVERIFY(writeFileBytes(indexFile, flipLastByte(readFileBytes(indexFile))));
    NotebookStore recovered;
    QVERIFY2(recovered.open(temp.path(), kPassword, QStringLiteral("device-a"), &error), qPrintable(error));
    QVERIFY(recovered.contains(noteId));
    QCOMPARE(recovered.name(), kNotebookName);
    QCOMPARE(readFileBytes(indexFile), indexBackup);

    // If both index copies are damaged, nothing is opened and no state is published.
    QVERIFY(writeFileBytes(indexFile, flipLastByte(readFileBytes(indexFile))));
    QVERIFY(writeFileBytes(indexFile + QStringLiteral(".bak"), flipLastByte(readFileBytes(indexFile + QStringLiteral(".bak")))));
    QVERIFY(!recovered.open(temp.path(), kPassword, QStringLiteral("device-a"), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!recovered.isOpen());
    QVERIFY(recovered.id().isEmpty());
    QVERIFY(recovered.name().isEmpty());
    QVERIFY(recovered.nodes(true).isEmpty());
    QVERIFY(recovered.masterPassword().isEmpty());

    // A header whose key check is damaged is rejected as well.
    QVERIFY(writeFileBytes(indexFile, indexBackup));
    const QString headerFile = temp.path() + QStringLiteral("/") + kHeaderName;
    const QByteArray headerBackup = readFileBytes(headerFile);
    QJsonObject header = readJsonObject(headerFile);
    QJsonObject keyCheck = header.value(QStringLiteral("keyCheck")).toObject();
    keyCheck.insert(QStringLiteral("ciphertext"),
                    QString::fromLatin1(flipLastByte(QByteArray::fromBase64(
                        keyCheck.value(QStringLiteral("ciphertext")).toString().toLatin1())).toBase64()));
    header.insert(QStringLiteral("keyCheck"), keyCheck);
    QVERIFY(writeFileBytes(headerFile, QJsonDocument(header).toJson(QJsonDocument::Compact)));
    QVERIFY(writeFileBytes(headerFile + QStringLiteral(".bak"), readFileBytes(headerFile)));
    QVERIFY(!recovered.open(temp.path(), kPassword, QStringLiteral("device-a"), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!recovered.isOpen());
    QVERIFY(writeFileBytes(headerFile, headerBackup));
    QVERIFY(writeFileBytes(headerFile + QStringLiteral(".bak"), headerBackup));
    QVERIFY2(recovered.open(temp.path(), kPassword, QStringLiteral("device-a"), &error), qPrintable(error));
    QCOMPARE(recovered.readNote(noteId, &error), kNoteContent);
}

void StorageTest::swappedHeaderIsRejected()
{
    QTemporaryDir temp;
    NotebookStore first;
    QString noteId;
    QString error;
    QVERIFY2(populate(&first, temp.path() + QStringLiteral("/first"), &noteId, &error), qPrintable(error));
    NotebookStore second;
    QString secondNote;
    QVERIFY2(populate(&second, temp.path() + QStringLiteral("/second"), &secondNote, &error), qPrintable(error));
    QVERIFY(first.id() != second.id());

    // Another notebook's header (even with the same password) has a different key and ID,
    // so it can never decrypt this notebook's index.
    const QByteArray foreign = readFileBytes(temp.path() + QStringLiteral("/first/") + kHeaderName);
    const QString headerFile = temp.path() + QStringLiteral("/second/") + kHeaderName;
    QVERIFY(writeFileBytes(headerFile, foreign));
    QVERIFY(writeFileBytes(headerFile + QStringLiteral(".bak"), foreign));
    QVERIFY(!second.open(temp.path() + QStringLiteral("/second"), kPassword, QStringLiteral("device-a"), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!second.isOpen());
    QVERIFY(second.nodes(true).isEmpty());
    QVERIFY(second.id().isEmpty());

    // A header without a valid notebook ID is rejected before any key is derived.
    QJsonObject broken = readJsonObject(temp.path() + QStringLiteral("/first/") + kHeaderName);
    broken.insert(QStringLiteral("id"), QStringLiteral("not a uuid"));
    QVERIFY(writeFileBytes(temp.path() + QStringLiteral("/first/") + kHeaderName, QJsonDocument(broken).toJson()));
    QVERIFY(!first.open(temp.path() + QStringLiteral("/first"), kPassword, QStringLiteral("device-a"), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!first.isOpen());
}

void StorageTest::remoteHeaderIsRejected()
{
    QString error;
    QVERIFY2(CryptoService::isAvailable(&error), qPrintable(error));
    QTemporaryDir temp;
    const QString id = QStringLiteral("01234567-89ab-cdef-8123-456789abcdef");

    // The remote WebDAV header is a different format and never opens a local notebook.
    CryptoService remote;
    QJsonObject remoteHeader;
    QVERIFY2(remote.createHeader(kPassword, &remoteHeader, &error), qPrintable(error));
    QCOMPARE(remoteHeader.value(QStringLiteral("format")).toString(), QStringLiteral("qoplin-encrypted-webdav"));
    remoteHeader.insert(QStringLiteral("id"), id);
    const QString headerFile = temp.path() + QStringLiteral("/") + kHeaderName;
    QVERIFY(writeFileBytes(headerFile, QJsonDocument(remoteHeader).toJson(QJsonDocument::Compact)));
    QVERIFY(writeFileBytes(headerFile + QStringLiteral(".bak"), readFileBytes(headerFile)));
    NotebookStore store;
    QVERIFY(!store.open(temp.path(), kPassword, QStringLiteral("device-a"), &error));
    QVERIFY2(error.contains(QStringLiteral("unsupported format")), qPrintable(error));
    QVERIFY(!store.isOpen());
    QVERIFY(store.masterPassword().isEmpty());
    QVERIFY(!QFileInfo::exists(temp.path() + QStringLiteral("/") + kIndexName));

    // The two header formats are domain separated in both directions.
    CryptoService crypto;
    QJsonObject localHeader;
    QVERIFY2(crypto.createHeader(kPassword, &localHeader, &error, true), qPrintable(error));
    QCOMPARE(localHeader.value(QStringLiteral("format")).toString(), QStringLiteral("qoplin-encrypted-local"));
    QCOMPARE(CryptoService::headerFormat(false), QStringLiteral("qoplin-encrypted-webdav"));
    QVERIFY(!crypto.initializeFromHeader(localHeader, kPassword, &error));
    QVERIFY(!crypto.isReady());
    QVERIFY(!crypto.initializeFromHeader(remoteHeader, kPassword, &error, true));
    QVERIFY(!crypto.isReady());
    QVERIFY(!crypto.initializeFromHeader(localHeader, QStringLiteral("wrong password"), &error, true));
    QVERIFY(!crypto.isReady());

    QVERIFY2(crypto.initializeFromHeader(localHeader, kPassword, &error, true), qPrintable(error));
    QVERIFY(crypto.isReady());
    crypto.clear();
    QVERIFY(!crypto.isReady());
    QByteArray cipherText;
    QVERIFY(!crypto.encrypt(QByteArrayLiteral("x"), QByteArrayLiteral("y"), &cipherText, &error));
    QVERIFY2(remote.initializeFromHeader(remoteHeader, kPassword, &error), qPrintable(error));
    QVERIFY(remote.isReady());
}

void StorageTest::legacyPlaintextNotebookIsRejected()
{
    QTemporaryDir temp;
    const QString root = temp.path();
    const QString id = QStringLiteral("01234567-89ab-cdef-8123-456789abcdef");
    const QString noteId = QStringLiteral("11111111-2222-4333-8444-555555555555");
    QVERIFY(QDir(root).mkpath(QStringLiteral(".qoplin/notes")));
    const QByteArray metadata = "{\"version\":1,\"id\":\"" + id.toUtf8()
                                + "\",\"name\":\"Legacy\",\"writerId\":\"device-a\"}";
    const QByteArray index = "{\"version\":1,\"nodes\":[{\"id\":\"" + noteId.toUtf8()
                             + "\",\"parentId\":\"\",\"type\":\"note\",\"title\":\"Legacy note\",\"fileName\":\""
                             + noteId.toUtf8() + ".md\",\"createdAt\":1,\"updatedAt\":1,\"writerId\":\"device-a\","
                               "\"sortOrder\":0,\"deleted\":false}]}";
    const QByteArray body = QByteArrayLiteral("legacy plaintext body");
    QVERIFY(writeFileBytes(root + QStringLiteral("/") + kHeaderName, metadata));
    QVERIFY(writeFileBytes(root + QStringLiteral("/.qoplin/index.json"), index));
    QVERIFY(writeFileBytes(root + QStringLiteral("/.qoplin/notes/") + noteId + QStringLiteral(".md"), body));

    // Old plaintext notebooks are unsupported. They are never opened and never converted.
    NotebookStore store;
    QString error;
    QVERIFY(!store.open(root, kPassword, QStringLiteral("device-a"), &error));
    QVERIFY2(error.contains(QStringLiteral("unencrypted")), qPrintable(error));
    QVERIFY(!store.isOpen());
    QVERIFY(store.id().isEmpty());
    QVERIFY(store.name().isEmpty());
    QVERIFY(store.nodes(true).isEmpty());
    QVERIFY(store.masterPassword().isEmpty());
    QCOMPARE(readFileBytes(root + QStringLiteral("/") + kHeaderName), metadata);
    QCOMPARE(readFileBytes(root + QStringLiteral("/.qoplin/index.json")), index);
    QCOMPARE(readFileBytes(root + QStringLiteral("/.qoplin/notes/") + noteId + QStringLiteral(".md")), body);
    QVERIFY(!QFileInfo::exists(root + QStringLiteral("/") + kIndexName));
    QVERIFY(!QFileInfo::exists(root + QStringLiteral("/") + kHeaderName + QStringLiteral(".bak")));
    QVERIFY(!store.contains(noteId));

    // The same holds without the plaintext metadata file.
    QVERIFY(QFile::remove(root + QStringLiteral("/") + kHeaderName));
    QVERIFY(!store.open(root, kPassword, QStringLiteral("device-a"), &error));
    QVERIFY2(error.contains(QStringLiteral("unencrypted")), qPrintable(error));
    QVERIFY(!store.isOpen());
    QVERIFY(!QFileInfo::exists(root + QStringLiteral("/") + kHeaderName));

    // And create() never writes into such a directory.
    QVERIFY(!store.create(root, QStringLiteral("Replacement"), QStringLiteral("device-a"), kPassword, &error));
    QVERIFY2(error.contains(QStringLiteral("already exists")), qPrintable(error));
    QVERIFY(!store.isOpen());
    QVERIFY(!QFileInfo::exists(root + QStringLiteral("/") + kHeaderName));
    QVERIFY(!QFileInfo::exists(root + QStringLiteral("/") + kIndexName));
    QCOMPARE(readFileBytes(root + QStringLiteral("/.qoplin/index.json")), index);
}

void StorageTest::rollbackRestoresCiphertext()
{
    QTemporaryDir temp;
    NotebookStore store;
    QString noteId;
    QString error;
    QVERIFY2(populate(&store, temp.path(), &noteId, &error), qPrintable(error));
    const QString internal = temp.path() + QStringLiteral("/.qoplin");
    const QString indexFile = internal + QStringLiteral("/index.bin");
    const QString noteFile = store.notePath(store.node(noteId));
    const QByteArray indexBytes = readFileBytes(indexFile);
    const QByteArray noteBytes = readFileBytes(noteFile);
    const NoteNode original = store.node(noteId);

    // A directory in place of the index makes every index commit fail.
    const auto blockIndex = [&internal, &indexFile] {
        QFile::remove(indexFile);
        QFile::remove(indexFile + QStringLiteral(".bak"));
        QDir(internal).mkdir(QStringLiteral("index.bin"));
    };

    // writeNote(): the new content is discarded and the previous ciphertext is restored byte for byte.
    blockIndex();
    QVERIFY(!store.writeNote(noteId, QStringLiteral("changed content"), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(QDir(internal).rmdir(QStringLiteral("index.bin")));
    QCOMPARE(store.node(noteId).updatedAt, original.updatedAt);
    QCOMPARE(readFileBytes(noteFile), noteBytes);
    QCOMPARE(store.readNote(noteId, &error), kNoteContent);

    // applyRemoteNode() of a new note: the object is removed again.
    NoteNode fresh;
    fresh.id = QStringLiteral("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
    fresh.type = NoteNode::Type::Note;
    fresh.title = QStringLiteral("Fresh remote note");
    blockIndex();
    QVERIFY(!store.applyRemoteNode(fresh, QStringLiteral("fresh remote body"), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(QDir(internal).rmdir(QStringLiteral("index.bin")));
    QVERIFY(!store.contains(fresh.id));
    QVERIFY(!QFileInfo::exists(temp.path() + QStringLiteral("/.qoplin/notes/") + fresh.id + QStringLiteral(".bin")));

    // applyRemoteNode() of an existing note: the previous ciphertext is restored.
    NoteNode changed = original;
    changed.title = QStringLiteral("Changed remotely");
    changed.updatedAt += 1000;
    blockIndex();
    QVERIFY(!store.applyRemoteNode(changed, QStringLiteral("remote change"), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(QDir(internal).rmdir(QStringLiteral("index.bin")));
    QCOMPARE(store.node(noteId).title, original.title);
    QCOMPARE(readFileBytes(noteFile), noteBytes);

    // Nothing but ciphertext was ever written, and the rolled-back files still open.
    for (const QString &file : notebookFiles(temp.path())) {
        if (!file.startsWith(kHeaderName))
            QVERIFY(!containsSecret(readFileBytes(temp.path() + QLatin1Char('/') + file), QStringLiteral("changed content")));
    }
    QVERIFY(writeFileBytes(indexFile, indexBytes));
    NotebookStore reopened;
    QVERIFY2(reopened.open(temp.path(), kPassword, QStringLiteral("device-a"), &error), qPrintable(error));
    QCOMPARE(reopened.readNote(noteId, &error), kNoteContent);
    QCOMPARE(reopened.node(noteId).title, kNoteTitle);
    QVERIFY(!reopened.contains(fresh.id));
}

void StorageTest::closeClearsSecrets()
{
    QTemporaryDir temp;
    NotebookStore store;
    QString noteId;
    QString error;
    QVERIFY2(populate(&store, temp.path(), &noteId, &error), qPrintable(error));
    QVERIFY(!store.masterPassword().isEmpty());

    store.close();
    QVERIFY(!store.isOpen());
    QVERIFY(store.masterPassword().isEmpty());
    QVERIFY(store.id().isEmpty());
    QVERIFY(store.name().isEmpty());
    QVERIFY(store.rootPath().isEmpty());
    QVERIFY(store.writerId().isEmpty());
    QVERIFY(store.nodes(true).isEmpty());
    QVERIFY(!store.contains(noteId));
    QVERIFY(!store.writeNote(noteId, QStringLiteral("x"), &error));
    QVERIFY(store.readNote(noteId, &error).isNull());
    QVERIFY(!error.isEmpty());

    // The files stay untouched and a closed store can be opened again.
    QVERIFY(!readFileBytes(temp.path() + QStringLiteral("/") + kIndexName).isEmpty());
    QVERIFY2(store.open(temp.path(), kPassword, QStringLiteral("device-a"), &error), qPrintable(error));
    QVERIFY(store.isOpen());
    QCOMPARE(store.readNote(noteId, &error), kNoteContent);

    // Destroying a store releases its key and password without touching the files.
    const QStringList files = notebookFiles(temp.path());
    QByteArray indexBytes;
    {
        auto owned = std::make_unique<NotebookStore>();
        QVERIFY2(owned->open(temp.path(), kPassword, QStringLiteral("device-a"), &error), qPrintable(error));
        QVERIFY(!owned->masterPassword().isEmpty());
        indexBytes = readFileBytes(temp.path() + QStringLiteral("/") + kIndexName);
    }
    QCOMPARE(notebookFiles(temp.path()), files);
    QCOMPARE(readFileBytes(temp.path() + QStringLiteral("/") + kIndexName), indexBytes);
}

void StorageTest::renameKeepsHeaderUnchanged()
{
    QTemporaryDir temp;
    NotebookStore store;
    QString noteId;
    QString error;
    QVERIFY2(populate(&store, temp.path(), &noteId, &error), qPrintable(error));
    const QString headerFile = temp.path() + QStringLiteral("/") + kHeaderName;
    const QByteArray headerBefore = readFileBytes(headerFile);

    // The name lives in the encrypted index only; renaming rewrites no header.
    QVERIFY(store.setName(QStringLiteral("Renamed 名称"), &error));
    QCOMPARE(store.name(), QStringLiteral("Renamed 名称"));
    QCOMPARE(readFileBytes(headerFile), headerBefore);
    QCOMPARE(readFileBytes(headerFile + QStringLiteral(".bak")), headerBefore);
    QVERIFY(!store.setName(QStringLiteral("  "), &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(store.name(), QStringLiteral("Renamed 名称"));

    for (const QString &file : notebookFiles(temp.path())) {
        if (file.startsWith(kHeaderName))
            continue;
        QVERIFY(!containsSecret(readFileBytes(temp.path() + QLatin1Char('/') + file), kNotebookName));
    }
    NotebookStore reopened;
    QVERIFY2(reopened.open(temp.path(), kPassword, QStringLiteral("device-a"), &error), qPrintable(error));
    QCOMPARE(reopened.name(), QStringLiteral("Renamed 名称"));
    QCOMPARE(reopened.readNote(noteId, &error), kNoteContent);
}

void StorageTest::createRejectsInvalidInput()
{
    QTemporaryDir temp;
    NotebookStore store;
    QString error;

    // No master password: nothing is created.
    QVERIFY(!store.create(temp.path() + QStringLiteral("/a"), QStringLiteral("No password"), kWriter, QString(), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!store.isOpen());
    QVERIFY(!QFileInfo::exists(temp.path() + QStringLiteral("/a")));

    // A path that is a file cannot hold a notebook: no header is written.
    QVERIFY(writeFileBytes(temp.path() + QStringLiteral("/b"), QByteArrayLiteral("file")));
    QVERIFY(!store.create(temp.path() + QStringLiteral("/b"), QStringLiteral("Blocked"), kWriter, kPassword, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!store.isOpen());
    QCOMPARE(readFileBytes(temp.path() + QStringLiteral("/b")), QByteArrayLiteral("file"));

    // An existing notebook is never overwritten and the store stays closed afterwards.
    QVERIFY2(store.create(temp.path() + QStringLiteral("/c"), QStringLiteral("First"), kWriter, kPassword, &error),
             qPrintable(error));
    QCOMPARE(store.writerId(), kWriter);
    const QByteArray header = readFileBytes(temp.path() + QStringLiteral("/c/") + kHeaderName);
    QVERIFY(!store.create(temp.path() + QStringLiteral("/c"), QStringLiteral("Second"), kWriter, kPassword, &error));
    QVERIFY2(error.contains(QStringLiteral("already exists")), qPrintable(error));
    QVERIFY(!store.isOpen());
    QVERIFY(store.masterPassword().isEmpty());
    QCOMPARE(readFileBytes(temp.path() + QStringLiteral("/c/") + kHeaderName), header);
    QCOMPARE(readJsonObject(temp.path() + QStringLiteral("/c/") + kHeaderName)
                 .value(QStringLiteral("name")).toString(), QString());
    QVERIFY(!containsSecret(header, QStringLiteral("First")));
    QVERIFY(!containsSecret(header, kWriter));

    // A writer ID is generated when none is given.
    NotebookStore anonymous;
    QVERIFY2(anonymous.create(temp.path() + QStringLiteral("/d"), QStringLiteral("Anonymous"), QString(), kPassword, &error),
             qPrintable(error));
    QVERIFY(!anonymous.writerId().isEmpty());
    QVERIFY(!containsSecret(readFileBytes(temp.path() + QStringLiteral("/d/") + kHeaderName), anonymous.writerId()));
}

QTEST_GUILESS_MAIN(StorageTest)
