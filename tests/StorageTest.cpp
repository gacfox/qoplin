#include "storage/AppSettings.h"
#include "storage/NotebookStore.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

class StorageTest : public QObject
{
    Q_OBJECT
private slots:
    void notebookLifecycle();
    void hierarchyAndTombstones();
    void remoteNodePreservesRevision();
    void indexBackupRecovery();
    void syncSettingsRoundTrip();
    void createWithSpecifiedId();
};

void StorageTest::notebookLifecycle()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    NotebookStore store;
    QString error;
    QVERIFY2(store.create(temp.path(), QStringLiteral("测试笔记"), QStringLiteral("device-a"), &error), qPrintable(error));
    QVERIFY(store.isOpen());
    const QString noteId = store.createNode(NoteNode::Type::Note, QString(), QStringLiteral("第一篇"), &error);
    QVERIFY2(!noteId.isEmpty(), qPrintable(error));
    QVERIFY(store.writeNote(noteId, QStringLiteral("# 标题\n\n你好，Qoplin。\n"), &error));
    const QString path = store.notePath(store.node(noteId));
    QVERIFY(QFileInfo::exists(path));
    QVERIFY(store.renameNode(noteId, QStringLiteral("新标题"), &error));
    QCOMPARE(store.notePath(store.node(noteId)), path);

    NotebookStore reopened;
    QVERIFY2(reopened.open(temp.path(), &error), qPrintable(error));
    QCOMPARE(reopened.name(), QStringLiteral("测试笔记"));
    QCOMPARE(reopened.node(noteId).title, QStringLiteral("新标题"));
    QCOMPARE(reopened.readNote(noteId, &error), QStringLiteral("# 标题\n\n你好，Qoplin。\n"));
    QVERIFY(error.isEmpty());
}

void StorageTest::hierarchyAndTombstones()
{
    QTemporaryDir temp;
    NotebookStore store;
    QString error;
    QVERIFY(store.create(temp.path(), QStringLiteral("Tree"), QStringLiteral("device-a"), &error));
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
}

void StorageTest::remoteNodePreservesRevision()
{
    QTemporaryDir temp;
    NotebookStore store;
    QString error;
    QVERIFY(store.create(temp.path(), QStringLiteral("Remote"), QStringLiteral("device-a"), &error));
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
    QCOMPARE(store.readNote(remote.id, &error), QStringLiteral("remote body"));
}

void StorageTest::indexBackupRecovery()
{
    QTemporaryDir temp;
    NotebookStore store;
    QString error;
    QVERIFY(store.create(temp.path(), QStringLiteral("Recovery"), QStringLiteral("device-a"), &error));
    const QString note = store.createNode(NoteNode::Type::Note, QString(), QStringLiteral("Recover me"), &error);
    QVERIFY(!note.isEmpty());
    const QString indexPath = temp.path() + QStringLiteral("/.qoplin/index.json");
    QVERIFY(QFileInfo::exists(indexPath + QStringLiteral(".bak")));
    {
        QFile file(indexPath);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("not json");
    }
    NotebookStore recovered;
    QVERIFY2(recovered.open(temp.path(), &error), qPrintable(error));
    QVERIFY(recovered.contains(note));
    QCOMPARE(recovered.node(note).title, QStringLiteral("Recover me"));
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
    expected.masterPassword = QStringLiteral("master password");
    const QString notebookId = QStringLiteral("01234567-89ab-cdef-8123-456789abcdef");
    settings.setSyncSettings(notebookId, expected);
    const SyncSettings actual = settings.syncSettings(notebookId);
    QCOMPARE(actual.enabled, expected.enabled);
    QCOMPARE(actual.endpoint, expected.endpoint);
    QCOMPARE(actual.remotePath, expected.remotePath);
    QCOMPARE(actual.userName, expected.userName);
    QCOMPARE(actual.serverPassword, expected.serverPassword);
    QCOMPARE(actual.masterPassword, expected.masterPassword);
}

void StorageTest::createWithSpecifiedId()
{
    QTemporaryDir temp;
    NotebookStore store;
    QString error;
    const QString id = QStringLiteral("01234567-89ab-cdef-8123-456789abcdef");
    QVERIFY2(store.createWithId(temp.path(), QStringLiteral("Imported"), id, QStringLiteral("device-a"), &error), qPrintable(error));
    QCOMPARE(store.id(), id);
    NotebookStore reopened;
    QVERIFY(reopened.open(temp.path(), &error));
    QCOMPARE(reopened.id(), id);
}

QTEST_GUILESS_MAIN(StorageTest)
#include "StorageTest.moc"
