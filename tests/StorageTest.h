#pragma once

#include <QObject>

class StorageTest : public QObject
{
    Q_OBJECT
private slots:
    void notebookLifecycle();
    void hierarchyAndTombstones();
    void remoteNodePreservesRevision();
    void indexBackupRecovery();
    void headerBackupRecovery();
    void syncSettingsRoundTrip();
    void createWithSpecifiedId();
    void headerIsNonSecret();
    void onDiskFilesAreEncrypted();
    void freshNoncePerEncryption();
    void wrongPasswordLeavesNoPartialState();
    void tamperedFilesAreRejected();
    void swappedHeaderIsRejected();
    void remoteHeaderIsRejected();
    void legacyPlaintextNotebookIsRejected();
    void rollbackRestoresCiphertext();
    void closeClearsSecrets();
    void renameKeepsHeaderUnchanged();
    void createRejectsInvalidInput();
};
