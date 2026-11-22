#include "CryptoService.h"

#include <QJsonDocument>

#include <sodium.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace {

constexpr int kFormatVersion = 1;
constexpr int kMaximumOpsLimit = 32;
constexpr quint64 kMaximumMemLimit = 1073741824;

const QByteArray kKeyCheckContext = QByteArrayLiteral("qoplin/header/key-check/v1");
const QByteArray kKeyCheckPlainText = QByteArrayLiteral("qoplin-key-check-v1");

void wipe(QByteArray &bytes)
{
    if (!bytes.isEmpty())
        sodium_memzero(bytes.data(), static_cast<std::size_t>(bytes.size()));
    bytes.clear();
}

bool readUnsigned(const QJsonObject &object, const QString &name, quint64 *value)
{
    const QJsonValue jsonValue = object.value(name);
    if (jsonValue.isString()) {
        bool ok = false;
        const quint64 parsed = jsonValue.toString().toULongLong(&ok);
        if (!ok)
            return false;
        *value = parsed;
        return true;
    }
    if (!jsonValue.isDouble())
        return false;
    const double parsed = jsonValue.toDouble();
    if (parsed < 0.0 || parsed > 9007199254740991.0 || parsed != std::floor(parsed))
        return false;
    *value = static_cast<quint64>(parsed);
    return true;
}

QByteArray decodeBase64Field(const QJsonObject &object, const QString &name, bool *ok)
{
    const QJsonValue value = object.value(name);
    if (!value.isString()) {
        *ok = false;
        return {};
    }
    const QByteArray encoded = value.toString().toLatin1();
    const QByteArray decoded = QByteArray::fromBase64(encoded, QByteArray::AbortOnBase64DecodingErrors);
    *ok = true;
    return decoded;
}

} // namespace

CryptoService::CryptoService() = default;

CryptoService::~CryptoService()
{
    clearKey();
}

bool CryptoService::isAvailable(QString *error)
{
    if (sodium_init() < 0) {
        if (error)
            *error = QStringLiteral("libsodium initialization failed.");
        return false;
    }
    if (error)
        error->clear();
    return true;
}

quint64 CryptoService::defaultOpsLimit()
{
    return crypto_pwhash_OPSLIMIT_INTERACTIVE;
}

quint64 CryptoService::defaultMemLimit()
{
    return crypto_pwhash_MEMLIMIT_INTERACTIVE;
}

bool CryptoService::setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

void CryptoService::clearKey()
{
    wipe(m_key);
}

bool CryptoService::deriveKey(const QString &masterPassword, const QByteArray &salt,
                              quint64 opsLimit, quint64 memLimit, QString *error)
{
    clearKey();
    if (masterPassword.isEmpty())
        return setError(error, QStringLiteral("The master password must not be empty."));
    if (salt.size() != crypto_pwhash_SALTBYTES)
        return setError(error, QStringLiteral("The Argon2id salt has an invalid length."));
    if (opsLimit < 1 || opsLimit > kMaximumOpsLimit
        || memLimit < 8192 || memLimit > kMaximumMemLimit
        || memLimit > static_cast<quint64>(std::numeric_limits<std::size_t>::max())) {
        return setError(error, QStringLiteral("The Argon2id parameters are outside the supported range."));
    }

    QByteArray password = masterPassword.toUtf8();
    m_key.resize(crypto_aead_xchacha20poly1305_ietf_KEYBYTES);
    const int result = crypto_pwhash(
        reinterpret_cast<unsigned char *>(m_key.data()),
        static_cast<unsigned long long>(m_key.size()), password.constData(),
        static_cast<unsigned long long>(password.size()),
        reinterpret_cast<const unsigned char *>(salt.constData()), opsLimit,
        static_cast<std::size_t>(memLimit), crypto_pwhash_ALG_ARGON2ID13);
    wipe(password);
    if (result != 0) {
        clearKey();
        return setError(error, QStringLiteral("Argon2id key derivation failed; the KDF parameters may require too much memory."));
    }
    if (error)
        error->clear();
    return true;
}

bool CryptoService::createHeader(const QString &masterPassword, QJsonObject *header,
                                 QString *error)
{
    if (!header)
        return setError(error, QStringLiteral("A header output object is required."));
    *header = QJsonObject();
    if (!isAvailable(error))
        return false;

    QByteArray salt(crypto_pwhash_SALTBYTES, Qt::Uninitialized);
    randombytes_buf(salt.data(), static_cast<std::size_t>(salt.size()));
    if (!deriveKey(masterPassword, salt, defaultOpsLimit(), defaultMemLimit(), error)) {
        wipe(salt);
        return false;
    }

    QByteArray encryptedCheck;
    if (!encrypt(kKeyCheckPlainText, kKeyCheckContext, &encryptedCheck, error)) {
        wipe(salt);
        clearKey();
        return false;
    }

    QJsonObject keyCheck;
    keyCheck.insert(QStringLiteral("nonce"),
                    QString::fromLatin1(encryptedCheck.left(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES).toBase64()));
    keyCheck.insert(QStringLiteral("ciphertext"),
                    QString::fromLatin1(encryptedCheck.mid(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES).toBase64()));
    wipe(encryptedCheck);

    QJsonObject result;
    result.insert(QStringLiteral("format"), QStringLiteral("qoplin-encrypted-webdav"));
    result.insert(QStringLiteral("version"), kFormatVersion);
    result.insert(QStringLiteral("kdf"), QStringLiteral("Argon2id"));
    result.insert(QStringLiteral("salt"), QString::fromLatin1(salt.toBase64()));
    result.insert(QStringLiteral("opsLimit"), QString::number(defaultOpsLimit()));
    result.insert(QStringLiteral("memLimit"), QString::number(defaultMemLimit()));
    result.insert(QStringLiteral("keyCheck"), keyCheck);
    wipe(salt);
    *header = result;
    if (error)
        error->clear();
    return true;
}

bool CryptoService::initializeFromHeader(const QJsonObject &header,
                                         const QString &masterPassword, QString *error)
{
    clearKey();
    if (header.value(QStringLiteral("format")).toString()
            != QStringLiteral("qoplin-encrypted-webdav")
        || header.value(QStringLiteral("version")).toInt(-1) != kFormatVersion
        || header.value(QStringLiteral("kdf")).toString() != QStringLiteral("Argon2id")) {
        return setError(error, QStringLiteral("The remote encryption header has an unsupported format or KDF."));
    }

    bool ok = false;
    QByteArray salt = decodeBase64Field(header, QStringLiteral("salt"), &ok);
    if (!ok || salt.size() != crypto_pwhash_SALTBYTES) {
        wipe(salt);
        return setError(error, QStringLiteral("The remote encryption header contains an invalid salt."));
    }
    quint64 opsLimit = 0;
    quint64 memLimit = 0;
    if (!readUnsigned(header, QStringLiteral("opsLimit"), &opsLimit)
        || !readUnsigned(header, QStringLiteral("memLimit"), &memLimit)) {
        wipe(salt);
        return setError(error, QStringLiteral("The remote encryption header contains invalid KDF parameters."));
    }
    if (!deriveKey(masterPassword, salt, opsLimit, memLimit, error)) {
        wipe(salt);
        return false;
    }
    wipe(salt);

    const QJsonValue checkValue = header.value(QStringLiteral("keyCheck"));
    if (!checkValue.isObject()) {
        clearKey();
        return setError(error, QStringLiteral("The remote encryption header has no key check."));
    }
    const QJsonObject check = checkValue.toObject();
    const QByteArray nonce = decodeBase64Field(check, QStringLiteral("nonce"), &ok);
    const QByteArray encrypted = decodeBase64Field(check, QStringLiteral("ciphertext"), &ok);
    if (!ok || nonce.size() != crypto_aead_xchacha20poly1305_ietf_NPUBBYTES
        || encrypted.size() < crypto_aead_xchacha20poly1305_ietf_ABYTES) {
        clearKey();
        return setError(error, QStringLiteral("The remote encryption header contains an invalid key check."));
    }

    QByteArray combined = nonce + encrypted;
    QByteArray plainText;
    const bool decrypted = decrypt(combined, kKeyCheckContext, &plainText, nullptr);
    wipe(combined);
    const bool valid = decrypted && plainText == kKeyCheckPlainText;
    wipe(plainText);
    if (!valid) {
        clearKey();
        return setError(error, QStringLiteral("The master password is incorrect or the remote header is damaged."));
    }
    if (error)
        error->clear();
    return true;
}

bool CryptoService::encrypt(const QByteArray &plainText, const QByteArray &associatedData,
                            QByteArray *cipherText, QString *error) const
{
    if (!cipherText)
        return setError(error, QStringLiteral("A ciphertext output buffer is required."));
    cipherText->clear();
    if (!isReady())
        return setError(error, QStringLiteral("The encryption key has not been initialized."));
    if (plainText.size() > std::numeric_limits<int>::max()
            - crypto_aead_xchacha20poly1305_ietf_NPUBBYTES
            - crypto_aead_xchacha20poly1305_ietf_ABYTES) {
        return setError(error, QStringLiteral("The plaintext is too large to encrypt."));
    }

    QByteArray nonce(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES, Qt::Uninitialized);
    randombytes_buf(nonce.data(), static_cast<std::size_t>(nonce.size()));
    cipherText->resize(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES + plainText.size()
                       + crypto_aead_xchacha20poly1305_ietf_ABYTES);
    std::copy(nonce.constBegin(), nonce.constEnd(), cipherText->begin());
    wipe(nonce);

    unsigned long long encryptedLength = 0;
    const int result = crypto_aead_xchacha20poly1305_ietf_encrypt(
        reinterpret_cast<unsigned char *>(cipherText->data()
                                         + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES),
        &encryptedLength, reinterpret_cast<const unsigned char *>(plainText.constData()),
        static_cast<unsigned long long>(plainText.size()),
        reinterpret_cast<const unsigned char *>(associatedData.constData()),
        static_cast<unsigned long long>(associatedData.size()), nullptr,
        reinterpret_cast<const unsigned char *>(cipherText->constData()),
        reinterpret_cast<const unsigned char *>(m_key.constData()));
    if (result != 0 || encryptedLength != static_cast<unsigned long long>(
            plainText.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES)) {
        cipherText->clear();
        return setError(error, QStringLiteral("XChaCha20-Poly1305 encryption failed."));
    }
    if (error)
        error->clear();
    return true;
}

bool CryptoService::decrypt(const QByteArray &cipherText, const QByteArray &associatedData,
                            QByteArray *plainText, QString *error) const
{
    if (!plainText)
        return setError(error, QStringLiteral("A plaintext output buffer is required."));
    plainText->clear();
    if (!isReady())
        return setError(error, QStringLiteral("The encryption key has not been initialized."));
    if (cipherText.size() < crypto_aead_xchacha20poly1305_ietf_NPUBBYTES
                            + crypto_aead_xchacha20poly1305_ietf_ABYTES) {
        return setError(error, QStringLiteral("The encrypted object is truncated."));
    }

    plainText->resize(cipherText.size() - crypto_aead_xchacha20poly1305_ietf_NPUBBYTES
                      - crypto_aead_xchacha20poly1305_ietf_ABYTES);
    unsigned long long plainTextLength = 0;
    const int result = crypto_aead_xchacha20poly1305_ietf_decrypt(
        reinterpret_cast<unsigned char *>(plainText->data()), &plainTextLength, nullptr,
        reinterpret_cast<const unsigned char *>(cipherText.constData()
                                               + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES),
        static_cast<unsigned long long>(cipherText.size()
                                       - crypto_aead_xchacha20poly1305_ietf_NPUBBYTES),
        reinterpret_cast<const unsigned char *>(associatedData.constData()),
        static_cast<unsigned long long>(associatedData.size()),
        reinterpret_cast<const unsigned char *>(cipherText.constData()),
        reinterpret_cast<const unsigned char *>(m_key.constData()));
    if (result != 0) {
        wipe(*plainText);
        return setError(error, QStringLiteral("The encrypted object failed authentication or is damaged."));
    }
    plainText->resize(static_cast<int>(plainTextLength));
    if (error)
        error->clear();
    return true;
}
