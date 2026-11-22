#include "SyncTest.h"

#include "sync/CryptoService.h"
#include "sync/WebDavClient.h"

#include <QJsonObject>
#include <QtTest>

void SyncTest::initTestCase()
{
    m_sodiumAvailable = CryptoService::isAvailable(&m_sodiumError);
}

void SyncTest::cryptoRoundTrip()
{
    if (!m_sodiumAvailable)
        QSKIP(qPrintable(QStringLiteral("libsodium is not available: %1").arg(m_sodiumError)));
    CryptoService crypto;
    QJsonObject header;
    QString error;
    QVERIFY2(crypto.createHeader(QStringLiteral("correct horse battery staple"), &header, &error),
             qPrintable(error));
    QVERIFY(crypto.isReady());
    QCOMPARE(header.value(QStringLiteral("version")).toInt(), 1);
    QCOMPARE(header.value(QStringLiteral("kdf")).toString(), QStringLiteral("Argon2id"));
    QCOMPARE(QByteArray::fromBase64(header.value(QStringLiteral("salt")).toString().toLatin1()).size(), 16);
    QVERIFY(header.value(QStringLiteral("keyCheck")).isObject());

    const QByteArray plainText = QByteArrayLiteral("Note metadata and UTF-8 markdown: \xF0\x9F\x93\x9D");
    const QByteArray context = QByteArrayLiteral("qoplin/object/v1:test");
    QByteArray cipherText;
    QVERIFY2(crypto.encrypt(plainText, context, &cipherText, &error), qPrintable(error));
    QVERIFY(cipherText.size() > plainText.size());
    QVERIFY(!cipherText.contains(plainText));

    QByteArray decrypted;
    QVERIFY2(crypto.decrypt(cipherText, context, &decrypted, &error), qPrintable(error));
    QCOMPARE(decrypted, plainText);

    QByteArray emptyDecrypted;
    QByteArray encryptedEmpty;
    QVERIFY(crypto.encrypt(QByteArray(), context, &encryptedEmpty, &error));
    QVERIFY(crypto.decrypt(encryptedEmpty, context, &emptyDecrypted, &error));
    QVERIFY(emptyDecrypted.isEmpty());
}

void SyncTest::encryptionUsesFreshNoncesAndContext()
{
    if (!m_sodiumAvailable)
        QSKIP(qPrintable(QStringLiteral("libsodium is not available: %1").arg(m_sodiumError)));
    CryptoService crypto;
    QJsonObject header;
    QString error;
    QVERIFY(crypto.createHeader(QStringLiteral("nonce test password"), &header, &error));

    QByteArray first;
    QByteArray second;
    QVERIFY(crypto.encrypt(QByteArrayLiteral("payload"), QByteArrayLiteral("context-a"), &first, &error));
    QVERIFY(crypto.encrypt(QByteArrayLiteral("payload"), QByteArrayLiteral("context-a"), &second, &error));
    QVERIFY(first != second);

    QByteArray plainText;
    QVERIFY(!crypto.decrypt(first, QByteArrayLiteral("context-b"), &plainText, &error));

    QByteArray altered = first;
    altered[altered.size() - 1] = static_cast<char>(altered.at(altered.size() - 1) ^ 0x01);
    QVERIFY(!crypto.decrypt(altered, QByteArrayLiteral("context-a"), &plainText, &error));
}

void SyncTest::wrongPasswordIsRejected()
{
    if (!m_sodiumAvailable)
        QSKIP(qPrintable(QStringLiteral("libsodium is not available: %1").arg(m_sodiumError)));
    CryptoService original;
    QJsonObject header;
    QString error;
    QVERIFY(original.createHeader(QStringLiteral("right password"), &header, &error));

    CryptoService restored;
    QVERIFY(restored.initializeFromHeader(header, QStringLiteral("right password"), &error));
    QVERIFY(restored.isReady());

    CryptoService wrong;
    QVERIFY(!wrong.initializeFromHeader(header, QStringLiteral("wrong password"), &error));
    QVERIFY(!wrong.isReady());
}

void SyncTest::webDavUrlsAreEscaped()
{
    SyncSettings settings;
    settings.endpoint = QStringLiteral("https://example.test/dav");
    settings.remotePath = QStringLiteral("Qoplin/My Notes");
    settings.userName = QStringLiteral("alice");
    WebDavClient client;
    client.configure(settings, QStringLiteral("runtime only"));

    const QUrl url = client.remoteUrl(QStringLiteral("objects/6E8122B4-31E4-4F06-996E-E58AA5B27285.bin"));
    QVERIFY(url.isValid());
    QCOMPARE(url.toEncoded(),
             QByteArrayLiteral("https://example.test/dav/Qoplin/My%20Notes/objects/6E8122B4-31E4-4F06-996E-E58AA5B27285.bin"));

    settings.remotePath = QStringLiteral("literal%2Fname");
    client.configure(settings, QStringLiteral("runtime only"));
    QVERIFY(client.remoteUrl().toEncoded().contains("literal%252Fname"));
}

void SyncTest::unsafeWebDavPathsAreRejected()
{
    SyncSettings settings;
    settings.endpoint = QStringLiteral("https://example.test/dav");
    settings.remotePath = QStringLiteral("Qoplin");
    WebDavClient client;
    client.configure(settings, QString());
    QVERIFY(!client.remoteUrl(QStringLiteral("../private")).isValid());
    QVERIFY(!client.remoteUrl(QStringLiteral("objects\\file.bin")).isValid());

    settings.remotePath = QStringLiteral("../outside");
    client.configure(settings, QString());
    QString error;
    QVERIFY(!client.isConfigured(&error));

    settings.remotePath = QStringLiteral("Qoplin");
    settings.endpoint = QStringLiteral("file:///tmp/not-webdav");
    client.configure(settings, QString());
    QVERIFY(!client.isConfigured(&error));

    settings.endpoint = QStringLiteral("https://user:secret@example.test/dav");
    client.configure(settings, QString());
    QVERIFY(!client.isConfigured(&error));
}

QTEST_GUILESS_MAIN(SyncTest)
