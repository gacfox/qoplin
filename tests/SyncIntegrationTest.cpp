#include "storage/NotebookStore.h"
#include "sync/CryptoService.h"
#include "sync/SyncWorker.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QHostAddress>
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
    QSet<QByteArray> collections;

    bool start()
    {
        if (!listen(QHostAddress::LocalHost, 0)) return false;
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                QTcpSocket *socket = nextPendingConnection();
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { receive(socket); });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
        return true;
    }

    QString endpoint() const { return QStringLiteral("http://127.0.0.1:%1/dav").arg(serverPort()); }

    bool containsInFiles(const QByteArray &text) const
    {
        for (const QByteArray &body : files) if (body.contains(text)) return true;
        return false;
    }

private:
    QHash<QTcpSocket *, QByteArray> m_buffers;

    void receive(QTcpSocket *socket)
    {
        QByteArray &buffer = m_buffers[socket];
        buffer.append(socket->readAll());
        const int headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) return;
        const QByteArray header = buffer.left(headerEnd);
        qsizetype contentLength = 0;
        const QList<QByteArray> lines = header.split('\n');
        for (QByteArray line : lines) {
            line = line.trimmed();
            if (line.toLower().startsWith("content-length:")) contentLength = line.mid(15).trimmed().toLongLong();
        }
        if (buffer.size() < headerEnd + 4 + contentLength) return;
        const QByteArray body = buffer.mid(headerEnd + 4, contentLength);
        const QByteArray requestLine = lines.isEmpty() ? QByteArray() : lines.first().trimmed();
        const QList<QByteArray> parts = requestLine.split(' ');
        if (parts.size() < 2) { respond(socket, 400, QByteArray()); return; }
        const QByteArray method = parts[0];
        const QByteArray path = parts[1];
        m_buffers.remove(socket);

        if (method == "PUT") {
            files.insert(path, body);
            respond(socket, 201, QByteArray());
        } else if (method == "GET") {
            respond(socket, files.contains(path) ? 200 : 404, files.value(path));
        } else if (method == "MKCOL") {
            collections.insert(path);
            respond(socket, 201, QByteArray());
        } else if (method == "PROPFIND") {
            const bool exists = collections.contains(path) || files.contains(path);
            const QByteArray xml = "<?xml version=\"1.0\"?><d:multistatus xmlns:d=\"DAV:\"></d:multistatus>";
            respond(socket, exists ? 207 : 404, exists ? xml : QByteArray());
        } else {
            respond(socket, 405, QByteArray());
        }
    }

    void respond(QTcpSocket *socket, int status, const QByteArray &body)
    {
        static const QHash<int, QByteArray> names{{200, "OK"}, {201, "Created"}, {207, "Multi-Status"},
            {400, "Bad Request"}, {404, "Not Found"}, {405, "Method Not Allowed"}};
        QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + " " + names.value(status) +
            "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        socket->write(response);
        socket->disconnectFromHost();
    }
};

bool copyDirectory(const QString &source, const QString &destination)
{
    QDir sourceDir(source);
    if (!QDir().mkpath(destination)) return false;
    const QFileInfoList entries = sourceDir.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const QFileInfo &entry : entries) {
        const QString target = QDir(destination).filePath(entry.fileName());
        if (entry.isDir()) { if (!copyDirectory(entry.absoluteFilePath(), target)) return false; }
        else if (!QFile::copy(entry.absoluteFilePath(), target)) return false;
    }
    return true;
}

struct SyncResult { bool success = false; QString message; int uploaded = 0; int downloaded = 0; };

SyncResult sync(NotebookStore &store, const QString &endpoint)
{
    SyncSettings settings;
    settings.enabled = true;
    settings.endpoint = endpoint;
    settings.remotePath = QStringLiteral("Qoplin");
    SyncWorker worker(&store);
    worker.setRequestTimeout(5000);
    worker.configure(settings, QString(), QStringLiteral("integration password"));
    SyncResult result;
    QObject::connect(&worker, &SyncWorker::finished, &worker,
        [&result](bool success, const QString &message, int uploaded, int downloaded) {
            result = {success, message, uploaded, downloaded};
        });
    worker.start();
    return result;
}
}

class SyncIntegrationTest : public QObject
{
    Q_OBJECT
private slots:
    void twoCopiesSynchronize();
};

void SyncIntegrationTest::twoCopiesSynchronize()
{
    QString sodiumError;
    if (!CryptoService::isAvailable(&sodiumError)) QSKIP(qPrintable(sodiumError));
    QTemporaryDir temp;
    MockWebDavServer server;
    QVERIFY(server.start());

    NotebookStore first;
    QString error;
    QVERIFY(first.create(temp.path() + QStringLiteral("/first"), QStringLiteral("Sync"), QStringLiteral("device-a"), QStringLiteral("sync master password"), &error));
    QVERIFY(copyDirectory(temp.path() + QStringLiteral("/first"), temp.path() + QStringLiteral("/second")));
    NotebookStore second;
    QVERIFY(second.open(temp.path() + QStringLiteral("/second"), QStringLiteral("sync master password"), QStringLiteral("device-b"), &error));
    second.setWriterId(QStringLiteral("device-b"));
    QCOMPARE(first.id(), second.id());

    const QString folder = first.createNode(NoteNode::Type::Folder, QString(), QStringLiteral("Shared"), &error);
    const QString note = first.createNode(NoteNode::Type::Note, folder, QStringLiteral("Document"), &error);
    QVERIFY(first.writeNote(note, QStringLiteral("from first"), &error));
    const QByteArray png = QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9Y9ZrXcAAAAASUVORK5CYII=");
    AssetRecord image;
    image.id = QStringLiteral("12345678-1234-4123-8123-123456789abc");
    image.mimeType = QStringLiteral("image/png");
    image.extension = QStringLiteral("png");
    image.originalName = QStringLiteral("pixel.png");
    QVERIFY2(first.writeAsset(image, png, &error), qPrintable(error));

    SyncResult result = sync(first, server.endpoint());
    QVERIFY2(result.success, qPrintable(result.message));
    QCOMPARE(result.uploaded, 3);
    QVERIFY(!server.containsInFiles("from first"));
    QVERIFY(!server.containsInFiles("Document"));
    QVERIFY(!server.containsInFiles(png));
    QVERIFY(!server.containsInFiles("pixel.png"));

    result = sync(second, server.endpoint());
    QVERIFY2(result.success, qPrintable(result.message));
    QCOMPARE(result.downloaded, 3);
    QCOMPARE(second.readNote(note, &error), QStringLiteral("from first"));
    QCOMPARE(second.node(folder).title, QStringLiteral("Shared"));
    QCOMPARE(second.readAsset(image.id, &error), png);
    QCOMPARE(second.asset(image.id).writerId, QStringLiteral("device-a"));

    // The remote payload is binary: a big-endian JSON length, full metadata, then PNG bytes.
    const QByteArray remotePrefix = "/dav/Qoplin/" + first.id().toUtf8();
    CryptoService remoteCrypto;
    const QJsonObject remoteHeader = QJsonDocument::fromJson(
        server.files.value(remotePrefix + "/header.json")).object();
    QVERIFY2(remoteCrypto.initializeFromHeader(remoteHeader, QStringLiteral("integration password"), &error),
             qPrintable(error));
    QByteArray envelope;
    QVERIFY2(remoteCrypto.decrypt(server.files.value(remotePrefix + "/assets/" + image.id.toUtf8() + ".bin"),
                                  "qoplin/asset/v1:" + image.id.toUtf8(), &envelope, &error),
             qPrintable(error));
    QVERIFY(envelope.size() > 4);
    const quint32 metadataLength = (static_cast<quint32>(static_cast<unsigned char>(envelope.at(0))) << 24)
        | (static_cast<quint32>(static_cast<unsigned char>(envelope.at(1))) << 16)
        | (static_cast<quint32>(static_cast<unsigned char>(envelope.at(2))) << 8)
        | static_cast<quint32>(static_cast<unsigned char>(envelope.at(3)));
    QVERIFY(metadataLength <= static_cast<quint32>(envelope.size() - 4));
    const QJsonObject metadata = QJsonDocument::fromJson(
        envelope.mid(4, static_cast<int>(metadataLength))).object();
    QCOMPARE(metadata.value(QStringLiteral("id")).toString(), image.id);
    QCOMPARE(metadata.value(QStringLiteral("originalName")).toString(), image.originalName);
    QCOMPARE(envelope.mid(4 + static_cast<int>(metadataLength)), png);

    QTest::qWait(2);
    QVERIFY(second.writeNote(note, QStringLiteral("newer from second"), &error));
    const QByteArray newerPng = png + QByteArrayLiteral("updated");
    QVERIFY(second.writeAsset(second.asset(image.id), newerPng, &error));
    result = sync(second, server.endpoint());
    QVERIFY2(result.success, qPrintable(result.message));
    QCOMPARE(result.uploaded, 2);
    result = sync(first, server.endpoint());
    QVERIFY2(result.success, qPrintable(result.message));
    QCOMPARE(result.downloaded, 2);
    QCOMPARE(first.readNote(note, &error), QStringLiteral("newer from second"));
    QCOMPARE(first.readAsset(image.id, &error), newerPng);
    QCOMPARE(first.asset(image.id).writerId, QStringLiteral("device-b"));

    AssetRecord deletedImage = second.asset(image.id);
    deletedImage.deleted = true;
    QVERIFY(second.writeAsset(deletedImage, QByteArray(), &error));
    QVERIFY(second.removeNode(folder, &error));
    result = sync(second, server.endpoint());
    QVERIFY2(result.success, qPrintable(result.message));
    QCOMPARE(result.uploaded, 3);
    result = sync(first, server.endpoint());
    QVERIFY2(result.success, qPrintable(result.message));
    QCOMPARE(result.downloaded, 3);
    QVERIFY(first.node(folder).deleted);
    QVERIFY(first.node(note).deleted);
    QVERIFY(first.asset(image.id).deleted);
    QVERIFY(first.readAsset(image.id, &error).isNull());
}

QTEST_GUILESS_MAIN(SyncIntegrationTest)
#include "SyncIntegrationTest.moc"
