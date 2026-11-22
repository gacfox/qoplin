#pragma once

#include <QObject>
#include <QString>

class SyncTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cryptoRoundTrip();
    void encryptionUsesFreshNoncesAndContext();
    void wrongPasswordIsRejected();
    void webDavUrlsAreEscaped();
    void unsafeWebDavPathsAreRejected();

private:
    bool m_sodiumAvailable = false;
    QString m_sodiumError;
};
