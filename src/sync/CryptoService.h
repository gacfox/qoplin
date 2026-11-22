#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

// libsodium cryptography, linked statically through vcpkg.
class CryptoService
{
public:
    CryptoService();
    ~CryptoService();

    CryptoService(const CryptoService &) = delete;
    CryptoService &operator=(const CryptoService &) = delete;

    static bool isAvailable(QString *error = nullptr);

    // Creates a new remote header with a random salt and an encrypted key check.
    bool createHeader(const QString &masterPassword, QJsonObject *header,
                      QString *error = nullptr);

    // Derives a key from an existing header and verifies its encrypted key check.
    bool initializeFromHeader(const QJsonObject &header,
                              const QString &masterPassword,
                              QString *error = nullptr);

    bool isReady() const { return !m_key.isEmpty(); }

    // The returned object is nonce || ciphertext || authentication tag.
    bool encrypt(const QByteArray &plainText, const QByteArray &associatedData,
                 QByteArray *cipherText, QString *error = nullptr) const;
    bool decrypt(const QByteArray &cipherText, const QByteArray &associatedData,
                 QByteArray *plainText, QString *error = nullptr) const;

    static quint64 defaultOpsLimit();
    static quint64 defaultMemLimit();

private:
    bool deriveKey(const QString &masterPassword, const QByteArray &salt,
                   quint64 opsLimit, quint64 memLimit, QString *error);
    void clearKey();
    static bool setError(QString *error, const QString &message);

    QByteArray m_key;
};
