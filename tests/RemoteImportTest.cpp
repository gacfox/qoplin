#include "storage/NotebookStore.h"
#include "sync/CryptoService.h"
#include "sync/RemoteCatalogWorker.h"
#include "sync/RemoteImportWorker.h"
#include "sync/WebDavClient.h"

#include <QDir>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

namespace {

class MockWebDavServer : public QTcpServer
{
public:
    QHash<QByteArray, QByteArray> files;
    QByteArray listing;
    QByteArray lastDepth;

    bool start()
    {
        if (!listen(QHostAddress::LocalHost, 0))
            return false;
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                QTcpSocket *socket = nextPendingConnection();
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { receive(socket); });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
        return true;
    }

    QString endpoint() const
    {
        return QStringLiteral("http://127.0.0.1:%1/dav").arg(serverPort());
    }

private:
    QHash<QTcpSocket *, QByteArray> m_buffers;

    void receive(QTcpSocket *socket)
    {
        QByteArray &buffer = m_buffers[socket];
        buffer.append(socket->readAll());
        const int headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0)
            return;
        const QByteArray header = buffer.left(headerEnd);
        qsizetype contentLength = 0;
        const QList<QByteArray> lines = header.split('\n');
        for (QByteArray line : lines) {
            line = line.trimmed();
            if (line.toLower().startsWith("content-length:"))
                contentLength = line.mid(15).trimmed().toLongLong();
            if (line.toLower().startsWith("depth:"))
                lastDepth = line.mid(6).trimmed();
        }
        if (buffer.size() < headerEnd + 4 + contentLength)
            return;
        const QByteArray body = buffer.mid(headerEnd + 4, contentLength);
        m_buffers.remove(socket);

        const QList<QByteArray> parts = lines.first().trimmed().split(' ');
        if (parts.size() < 2) {
            respond(socket, 400, {});
            return;
        }
        const QByteArray method = parts[0];
        const QByteArray path = parts[1];
        if (method == "GET")
            respond(socket, files.contains(path) ? 200 : 404, files.value(path));
        else if (method == "PROPFIND")
            respond(socket, 207, path == "/dav/Qoplin" || path == "/dav/Qoplin/"
                                   ? listing : QByteArray());
        else if (method == "PUT") {
            files.insert(path, body);
            respond(socket, 201, {});
        } else {
            respond(socket, 405, {});
        }
    }

    void respond(QTcpSocket *socket, int status, const QByteArray &body)
    {
        static const QHash<int, QByteArray> names{
            {200, "OK"}, {201, "Created"}, {207, "Multi-Status"},
            {400, "Bad Request"}, {404, "Not Found"}, {405, "Method Not Allowed"}};
        const QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + " "
            + names.value(status) + "\r\nContent-Length: " + QByteArray::number(body.size())
            + "\r\nConnection: close\r\n\r\n" + body;
        socket->write(response);
        socket->disconnectFromHost();
    }
};

struct ImportResult
{
    bool success = false;
    QString message;
    QString rootPath;
    int importedObjects = 0;
};

QJsonObject nodeJson(const NoteNode &node)
{
    return {
        {QStringLiteral("id"), node.id},
        {QStringLiteral("parentId"), node.parentId},
        {QStringLiteral("type"), node.isFolder() ? QStringLiteral("folder") : QStringLiteral("note")},
        {QStringLiteral("title"), node.title},
        {QStringLiteral("fileName"), node.fileName},
        {QStringLiteral("createdAt"), QString::number(node.createdAt)},
        {QStringLiteral("updatedAt"), QString::number(node.updatedAt)},
        {QStringLiteral("writerId"), node.writerId},
        {QStringLiteral("sortOrder"), node.sortOrder},
        {QStringLiteral("deleted"), node.deleted}
    };
}

bool seedNotebook(MockWebDavServer *server, const QString &notebookId,
                  const QString &password, const QString &notebookName,
                  bool corruptObject, QString *error)
{
    const QString folderId = QStringLiteral("ffffffff-ffff-4fff-bfff-ffffffffffff");
    const QString noteId = QStringLiteral("00000000-0000-4000-8000-000000000001");
    NoteNode folder;
    folder.id = folderId;
    folder.type = NoteNode::Type::Folder;
    folder.title = QStringLiteral("Remote folder");
    folder.createdAt = 10;
    folder.updatedAt = 20;
    folder.writerId = QStringLiteral("remote-writer");
    NoteNode note;
    note.id = noteId;
    note.parentId = folderId;
    note.type = NoteNode::Type::Note;
    note.title = QStringLiteral("Remote note");
    note.fileName = noteId + QStringLiteral(".md");
    note.createdAt = 11;
    note.updatedAt = 21;
    note.writerId = QStringLiteral("remote-writer");

    CryptoService crypto;
    QJsonObject header;
    if (!crypto.createHeader(password, &header, error))
        return false;
    const QJsonArray manifestNodes{
        QJsonObject{{QStringLiteral("id"), noteId}, {QStringLiteral("updatedAt"), QString::number(21)},
                    {QStringLiteral("writerId"), note.writerId}},
        QJsonObject{{QStringLiteral("id"), folderId}, {QStringLiteral("updatedAt"), QString::number(20)},
                    {QStringLiteral("writerId"), folder.writerId}}};
    const QJsonObject manifest{
        {QStringLiteral("version"), 1}, {QStringLiteral("notebookName"), notebookName},
        {QStringLiteral("nodes"), manifestNodes}};
    QByteArray encryptedManifest;
    if (!crypto.encrypt(QJsonDocument(manifest).toJson(QJsonDocument::Compact),
                        QByteArrayLiteral("qoplin/manifest/v1"), &encryptedManifest, error)) {
        return false;
    }

    const QByteArray prefix = "/dav/Qoplin/" + notebookId.toUtf8();
    server->files.insert(prefix + "/header.json", QJsonDocument(header).toJson(QJsonDocument::Compact));
    server->files.insert(prefix + "/manifest.bin", encryptedManifest);
    for (const NoteNode &node : {folder, note}) {
        const QJsonObject payload{
            {QStringLiteral("version"), 1}, {QStringLiteral("node"), nodeJson(node)},
            {QStringLiteral("content"), node.isNote() ? QStringLiteral("Encrypted markdown") : QString()}};
        QByteArray encrypted;
        if (!crypto.encrypt(QJsonDocument(payload).toJson(QJsonDocument::Compact),
                            "qoplin/object/v1:" + node.id.toUtf8(), &encrypted, error)) {
            return false;
        }
        if (corruptObject && node.isNote())
            encrypted[encrypted.size() - 1] = static_cast<char>(encrypted.at(encrypted.size() - 1) ^ 1);
        server->files.insert(prefix + "/objects/" + node.id.toUtf8() + ".bin", encrypted);
    }
    return true;
}

ImportResult import(RemoteImportWorker &worker)
{
    ImportResult result;
    QObject::connect(&worker, &RemoteImportWorker::finished, &worker,
        [&result](bool success, const QString &message, const QString &rootPath, int count) {
            result = {success, message, rootPath, count};
        });
    worker.start();
    return result;
}

} // namespace

class RemoteImportTest : public QObject
{
    Q_OBJECT

private slots:
    void catalogListsImmediateCollections();
    void importsEncryptedNotebook();
    void failedImportCleansStaging();
    void cancelledImportCleansStaging();
};

void RemoteImportTest::catalogListsImmediateCollections()
{
    MockWebDavServer server;
    QVERIFY(server.start());
    const QString id = QStringLiteral("11111111-1111-4111-8111-111111111111");
    server.listing = QByteArrayLiteral(
        "<?xml version=\"1.0\"?><d:multistatus xmlns:d=\"DAV:\">"
        "<d:response><d:href>/dav/Qoplin/</d:href><d:propstat><d:prop>"
        "<d:resourcetype><d:collection/></d:resourcetype></d:prop></d:propstat></d:response>"
        "<d:response><d:href>/dav/Qoplin/My%20Notes/</d:href><d:propstat><d:prop>"
        "<d:resourcetype><d:collection/></d:resourcetype></d:prop></d:propstat></d:response>"
        "<d:response><d:href>/dav/Qoplin/") + id.toUtf8() + QByteArrayLiteral(
        "/</d:href><d:propstat><d:prop><d:resourcetype><d:collection/></d:resourcetype>"
        "</d:prop></d:propstat></d:response>"
        "<d:response><d:href>/dav/Qoplin/file.txt</d:href><d:propstat><d:prop>"
        "<d:resourcetype/></d:prop></d:propstat></d:response>"
        "<d:response><d:href>/dav/Qoplin/My%20Notes/nested/</d:href><d:propstat><d:prop>"
        "<d:resourcetype><d:collection/></d:resourcetype></d:prop></d:propstat></d:response>"
        "</d:multistatus>");

    SyncSettings settings;
    settings.endpoint = server.endpoint();
    settings.remotePath = QStringLiteral("Qoplin");
    RemoteCatalogWorker worker;
    worker.setRequestTimeout(5000);
    worker.configure(settings, QString());
    bool success = false;
    QString message;
    QStringList ids;
    connect(&worker, &RemoteCatalogWorker::finished, &worker,
            [&](bool ok, const QString &text, const QStringList &notebookIds) {
                success = ok;
                message = text;
                ids = notebookIds;
            });
    worker.start();
    QVERIFY2(success, qPrintable(message));
    QCOMPARE(server.lastDepth, QByteArrayLiteral("1"));
    QCOMPARE(ids, QStringList({id, QStringLiteral("My Notes")}));

    server.listing = QByteArrayLiteral(
        "<!DOCTYPE d [<!ENTITY unsafe \"value\">]>"
        "<d:multistatus xmlns:d=\"DAV:\"></d:multistatus>");
    ids.clear();
    message.clear();
    worker.start();
    QVERIFY(!success);
    QVERIFY(ids.isEmpty());
    QVERIFY2(message.contains(QStringLiteral("not valid XML")), qPrintable(message));
}

void RemoteImportTest::importsEncryptedNotebook()
{
    QString error;
    if (!CryptoService::isAvailable(&error))
        QSKIP(qPrintable(error));
    QTemporaryDir temp;
    MockWebDavServer server;
    QVERIFY(server.start());
    const QString id = QStringLiteral("22222222-2222-4222-8222-222222222222");
    QVERIFY2(seedNotebook(&server, id, QStringLiteral("master password"),
                          QStringLiteral("Manifest name"), false, &error), qPrintable(error));

    SyncSettings settings;
    settings.endpoint = server.endpoint();
    settings.remotePath = QStringLiteral("Qoplin");
    RemoteImportWorker worker;
    worker.setRequestTimeout(5000);
    const QString target = temp.path() + QStringLiteral("/imported");
    worker.configure(settings, QString(), QStringLiteral("master password"), id, target,
                     QString(), QStringLiteral("local-writer"));
    const ImportResult result = import(worker);
    QVERIFY2(result.success, qPrintable(result.message));
    QCOMPARE(result.rootPath, target);
    QCOMPARE(result.importedObjects, 2);

    NotebookStore store;
    QVERIFY2(store.open(target, &error), qPrintable(error));
    QCOMPARE(store.id(), id);
    QCOMPARE(store.name(), QStringLiteral("Manifest name"));
    QCOMPARE(store.node(QStringLiteral("ffffffff-ffff-4fff-bfff-ffffffffffff")).title,
             QStringLiteral("Remote folder"));
    QCOMPARE(store.readNote(QStringLiteral("00000000-0000-4000-8000-000000000001"), &error),
             QStringLiteral("Encrypted markdown"));

    const ImportResult second = import(worker);
    QVERIFY(!second.success);
    QCOMPARE(second.importedObjects, 0);
    NotebookStore original;
    QVERIFY(original.open(target, &error));
    QCOMPARE(original.name(), QStringLiteral("Manifest name"));
}

void RemoteImportTest::failedImportCleansStaging()
{
    QString error;
    if (!CryptoService::isAvailable(&error))
        QSKIP(qPrintable(error));
    QTemporaryDir temp;
    MockWebDavServer server;
    QVERIFY(server.start());
    const QString id = QStringLiteral("33333333-3333-4333-8333-333333333333");
    QVERIFY2(seedNotebook(&server, id, QStringLiteral("master password"),
                          QStringLiteral("Broken"), true, &error), qPrintable(error));

    SyncSettings settings;
    settings.endpoint = server.endpoint();
    settings.remotePath = QStringLiteral("Qoplin");
    RemoteImportWorker worker;
    worker.setRequestTimeout(5000);
    const QString target = temp.path() + QStringLiteral("/broken");
    worker.configure(settings, QString(), QStringLiteral("master password"), id, target,
                     QStringLiteral("Requested name"), QStringLiteral("local-writer"));
    const ImportResult result = import(worker);
    QVERIFY(!result.success);
    QVERIFY(!QFileInfo::exists(target));
    QCOMPARE(result.importedObjects, 0);
    QVERIFY(QDir(temp.path()).entryList({QStringLiteral(".qoplin-import-*")},
                                        QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
}

void RemoteImportTest::cancelledImportCleansStaging()
{
    QString error;
    if (!CryptoService::isAvailable(&error))
        QSKIP(qPrintable(error));
    QTemporaryDir temp;
    MockWebDavServer server;
    QVERIFY(server.start());
    const QString id = QStringLiteral("44444444-4444-4444-8444-444444444444");
    QVERIFY2(seedNotebook(&server, id, QStringLiteral("master password"),
                          QStringLiteral("Cancelled"), false, &error), qPrintable(error));

    SyncSettings settings;
    settings.endpoint = server.endpoint();
    settings.remotePath = QStringLiteral("Qoplin");
    RemoteImportWorker worker;
    worker.setRequestTimeout(5000);
    const QString target = temp.path() + QStringLiteral("/cancelled");
    worker.configure(settings, QString(), QStringLiteral("master password"), id, target,
                     QString(), QStringLiteral("local-writer"));
    connect(&worker, &RemoteImportWorker::progress, &worker,
            [&worker](const QString &message) {
                if (message.startsWith(QStringLiteral("Imported ")))
                    worker.cancel();
            });
    const ImportResult result = import(worker);
    QVERIFY(!result.success);
    QVERIFY2(result.message.contains(QStringLiteral("cancelled")), qPrintable(result.message));
    QVERIFY(!QFileInfo::exists(target));
    QCOMPARE(result.importedObjects, 0);
    QVERIFY(QDir(temp.path()).entryList({QStringLiteral(".qoplin-import-*")},
                                        QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
}

QTEST_GUILESS_MAIN(RemoteImportTest)
#include "RemoteImportTest.moc"
