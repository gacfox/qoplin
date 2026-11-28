#include "RemoteImportWorker.h"

#include "CryptoService.h"
#include "WebDavClient.h"
#include "models/NoteNode.h"
#include "storage/NotebookStore.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QScopedPointer>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace {

constexpr int kManifestVersion = 1;
const QByteArray kManifestContext = QByteArrayLiteral("qoplin/manifest/v1");

struct RemoteEntry
{
    QString id;
    qint64 updatedAt = 0;
    QString writerId;
};

struct PendingImport
{
    NoteNode node;
    QString content;
};

bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

QString responseFailure(const QString &action, const WebDavResponse &response)
{
    if (response.hasHttpStatus()) {
        const QString status = QStringLiteral("HTTP %1").arg(response.statusCode);
        return response.errorString.isEmpty()
            ? QStringLiteral("%1 failed with %2.").arg(action, status)
            : QStringLiteral("%1 failed with %2: %3").arg(action, status, response.errorString);
    }
    const QString detail = response.errorString.isEmpty()
        ? QStringLiteral("a network error") : response.errorString;
    return QStringLiteral("%1 failed: %2").arg(action, detail);
}

bool successful(const WebDavResponse &response, const QString &action, QString *error)
{
    if (response.isSuccess())
        return true;
    return fail(error, responseFailure(action, response));
}

bool parseObject(const QByteArray &json, QJsonObject *object, const QString &description,
                 QString *error)
{
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return fail(error, QStringLiteral("%1 is not valid JSON.").arg(description));
    *object = document.object();
    return true;
}

bool readInt64(const QJsonObject &object, const QString &name, qint64 *value)
{
    const QJsonValue jsonValue = object.value(name);
    if (jsonValue.isString()) {
        bool ok = false;
        const qint64 parsed = jsonValue.toString().toLongLong(&ok);
        if (!ok)
            return false;
        *value = parsed;
        return true;
    }
    if (!jsonValue.isDouble())
        return false;
    const double parsed = jsonValue.toDouble();
    if (parsed < -9007199254740991.0 || parsed > 9007199254740991.0
        || parsed != std::floor(parsed)) {
        return false;
    }
    *value = static_cast<qint64>(parsed);
    return true;
}

bool validUuid(const QString &id)
{
    QString canonical = id;
    if (canonical.size() == 38 && canonical.startsWith(QLatin1Char('{'))
        && canonical.endsWith(QLatin1Char('}'))) {
        canonical = canonical.mid(1, 36);
    }
    if (canonical.size() != 36)
        return false;
    const QUuid uuid = QUuid::fromString(canonical);
    return !uuid.isNull()
        && uuid.toString(QUuid::WithoutBraces).compare(canonical, Qt::CaseInsensitive) == 0;
}

QString normalizedUuid(const QString &id)
{
    return QUuid::fromString(id).toString(QUuid::WithoutBraces).toLower();
}

bool safeFileName(const QString &fileName)
{
    return !fileName.isEmpty() && fileName != QStringLiteral(".")
        && fileName != QStringLiteral("..") && !fileName.contains(QLatin1Char('/'))
        && !fileName.contains(QLatin1Char('\\')) && !fileName.contains(QChar::Null);
}

QByteArray objectContext(const QString &id)
{
    return QByteArrayLiteral("qoplin/object/v1:") + id.toUtf8();
}

QString objectPath(const QString &id)
{
    return QStringLiteral("objects/%1.bin").arg(id);
}

QByteArray assetContext(const QString &id)
{
    return QByteArrayLiteral("qoplin/asset/v1:") + id.toUtf8();
}

QString remoteAssetPath(const QString &id)
{
    return QStringLiteral("assets/%1.bin").arg(id);
}

bool nodeFromJson(const QJsonObject &object, const QString &expectedId,
                  NoteNode *node, QString *error)
{
    if (!object.value(QStringLiteral("id")).isString()
        || !object.value(QStringLiteral("parentId")).isString()
        || !object.value(QStringLiteral("title")).isString()
        || !object.value(QStringLiteral("fileName")).isString()
        || !object.value(QStringLiteral("writerId")).isString()
        || !object.value(QStringLiteral("deleted")).isBool()) {
        return fail(error, QStringLiteral("A remote object has invalid node metadata."));
    }

    NoteNode result;
    result.id = object.value(QStringLiteral("id")).toString();
    result.parentId = object.value(QStringLiteral("parentId")).toString();
    result.title = object.value(QStringLiteral("title")).toString();
    result.fileName = object.value(QStringLiteral("fileName")).toString();
    result.writerId = object.value(QStringLiteral("writerId")).toString();
    result.deleted = object.value(QStringLiteral("deleted")).toBool();
    const QString type = object.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("folder"))
        result.type = NoteNode::Type::Folder;
    else if (type == QStringLiteral("note"))
        result.type = NoteNode::Type::Note;
    else
        return fail(error, QStringLiteral("A remote object has an unknown node type."));

    if (!readInt64(object, QStringLiteral("createdAt"), &result.createdAt)
        || !readInt64(object, QStringLiteral("updatedAt"), &result.updatedAt)
        || result.createdAt < 0 || result.updatedAt < 0) {
        return fail(error, QStringLiteral("A remote object has invalid timestamps."));
    }
    const QJsonValue sortOrder = object.value(QStringLiteral("sortOrder"));
    if (!sortOrder.isDouble() || sortOrder.toDouble() != std::floor(sortOrder.toDouble())
        || sortOrder.toDouble() < std::numeric_limits<int>::min()
        || sortOrder.toDouble() > std::numeric_limits<int>::max()) {
        return fail(error, QStringLiteral("A remote object has an invalid sort order."));
    }
    result.sortOrder = sortOrder.toInt();

    if (!validUuid(result.id) || result.id != expectedId
        || (!result.parentId.isEmpty() && !validUuid(result.parentId))
        || result.parentId == result.id
        || (result.isNote() && !safeFileName(result.fileName))
        || (result.isFolder() && !result.fileName.isEmpty() && !safeFileName(result.fileName))) {
        return fail(error, QStringLiteral("A remote object has unsafe or inconsistent node identifiers."));
    }

    *node = result;
    return true;
}

bool assetFromJson(const QJsonObject &object, const QString &expectedId,
                   AssetRecord *asset, QString *error)
{
    if (!object.value(QStringLiteral("id")).isString()
        || !object.value(QStringLiteral("mimeType")).isString()
        || !object.value(QStringLiteral("extension")).isString()
        || !object.value(QStringLiteral("originalName")).isString()
        || !object.value(QStringLiteral("sha256")).isString()
        || !object.value(QStringLiteral("writerId")).isString()
        || !object.value(QStringLiteral("deleted")).isBool()) {
        return fail(error, QStringLiteral("A remote asset has invalid metadata."));
    }

    AssetRecord result;
    result.id = object.value(QStringLiteral("id")).toString();
    result.mimeType = object.value(QStringLiteral("mimeType")).toString();
    result.extension = object.value(QStringLiteral("extension")).toString();
    result.originalName = object.value(QStringLiteral("originalName")).toString();
    result.sha256 = object.value(QStringLiteral("sha256")).toString();
    result.writerId = object.value(QStringLiteral("writerId")).toString();
    result.deleted = object.value(QStringLiteral("deleted")).toBool();
    if (!readInt64(object, QStringLiteral("size"), &result.size)
        || !readInt64(object, QStringLiteral("createdAt"), &result.createdAt)
        || !readInt64(object, QStringLiteral("updatedAt"), &result.updatedAt)
        || result.size < 0 || result.createdAt < 0 || result.updatedAt < 0) {
        return fail(error, QStringLiteral("A remote asset has invalid sizes or timestamps."));
    }
    if (!validUuid(result.id) || result.id != expectedId)
        return fail(error, QStringLiteral("A remote asset has an unsafe or inconsistent identifier."));

    *asset = result;
    return true;
}

bool decodeAssetEnvelope(const QByteArray &plainText, const QString &expectedId,
                         AssetRecord *asset, QByteArray *data, QString *error)
{
    if (plainText.size() < 4)
        return fail(error, QStringLiteral("A remote asset has a truncated binary envelope."));
    const auto byte = [&plainText](int offset) {
        return static_cast<quint32>(static_cast<unsigned char>(plainText.at(offset)));
    };
    const quint32 metadataLength = (byte(0) << 24) | (byte(1) << 16) | (byte(2) << 8) | byte(3);
    if (metadataLength > static_cast<quint32>(plainText.size() - 4))
        return fail(error, QStringLiteral("A remote asset has an invalid metadata length."));
    QJsonObject metadata;
    if (!parseObject(plainText.mid(4, static_cast<int>(metadataLength)), &metadata,
                     QStringLiteral("Remote asset metadata"), error)
        || !assetFromJson(metadata, expectedId, asset, error)) {
        return false;
    }
    *data = plainText.mid(4 + static_cast<int>(metadataLength));
    return true;
}

bool parseManifestEntries(const QJsonArray &array, QHash<QString, RemoteEntry> *entries,
                          QString *error)
{
    entries->clear();
    QSet<QString> normalizedIds;
    for (const QJsonValue &value : array) {
        if (!value.isObject())
            return fail(error, QStringLiteral("The remote manifest contains an invalid entry."));
        const QJsonObject jsonEntry = value.toObject();
        if (!jsonEntry.value(QStringLiteral("id")).isString()
            || !jsonEntry.value(QStringLiteral("writerId")).isString()) {
            return fail(error, QStringLiteral("The remote manifest contains invalid metadata."));
        }
        RemoteEntry entry;
        entry.id = jsonEntry.value(QStringLiteral("id")).toString();
        entry.writerId = jsonEntry.value(QStringLiteral("writerId")).toString();
        if (!validUuid(entry.id)
            || !readInt64(jsonEntry, QStringLiteral("updatedAt"), &entry.updatedAt)
            || entry.updatedAt < 0 || entries->contains(entry.id)
            || normalizedIds.contains(normalizedUuid(entry.id))) {
            return fail(error, QStringLiteral("The remote manifest contains an unsafe, invalid, or duplicate entry."));
        }
        entries->insert(entry.id, entry);
        normalizedIds.insert(normalizedUuid(entry.id));
    }
    return true;
}

bool parseManifest(const QJsonObject &object, QHash<QString, RemoteEntry> *entries,
                   QHash<QString, RemoteEntry> *assetEntries, QString *notebookName, QString *error)
{
    if (object.value(QStringLiteral("version")).toInt(-1) != kManifestVersion
        || !object.value(QStringLiteral("nodes")).isArray()) {
        return fail(error, QStringLiteral("The remote manifest has an unsupported format."));
    }

    notebookName->clear();
    if (object.contains(QStringLiteral("notebookName"))) {
        if (!object.value(QStringLiteral("notebookName")).isString())
            return fail(error, QStringLiteral("The remote manifest has an invalid notebook name."));
        *notebookName = object.value(QStringLiteral("notebookName")).toString();
    }
    const QJsonValue assets = object.value(QStringLiteral("assets"));
    if (!assets.isUndefined() && !assets.isArray())
        return fail(error, QStringLiteral("The remote manifest has an invalid asset list."));
    return parseManifestEntries(object.value(QStringLiteral("nodes")).toArray(), entries, error)
        && parseManifestEntries(assets.toArray(), assetEntries, error);
}

bool pathEntryExists(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() || info.isSymLink();
}

} // namespace

RemoteImportWorker::RemoteImportWorker(QObject *parent)
    : QObject(parent)
    , m_client(new WebDavClient(this))
{
}

RemoteImportWorker::~RemoteImportWorker()
{
    m_serverPassword.fill(QChar::Null);
    m_masterPassword.fill(QChar::Null);
    m_serverPassword.clear();
    m_masterPassword.clear();
}

void RemoteImportWorker::configure(const SyncSettings &baseSettings,
                                   const QString &serverPassword,
                                   const QString &masterPassword,
                                   const QString &remoteNotebookId,
                                   const QString &localRootPath,
                                   const QString &requestedLocalName,
                                   const QString &writerId)
{
    if (m_running.load())
        return;
    m_settings = baseSettings;
    m_serverPassword = serverPassword;
    m_masterPassword = masterPassword;
    m_remoteNotebookId = remoteNotebookId;
    m_localRootPath = localRootPath;
    m_requestedLocalName = requestedLocalName;
    m_writerId = writerId;
}

void RemoteImportWorker::setRequestTimeout(int milliseconds)
{
    m_client->setTimeout(milliseconds);
}

void RemoteImportWorker::cancel()
{
    m_cancelled.store(true);
    m_client->cancel();
}

void RemoteImportWorker::start()
{
    if (m_running.exchange(true)) {
        emit finished(false, QStringLiteral("Remote notebook import is already running."), {}, 0);
        return;
    }

    int importedObjects = 0;
    QString stagingPath;
    QString targetPath;
    m_cancelled.store(false);
    m_client->resetCancellation();

    const auto complete = [this, &stagingPath](bool success, const QString &message,
                                                const QString &rootPath, int count) {
        QString finalMessage = message;
        if (!success && !stagingPath.isEmpty()) {
            if (!QDir(stagingPath).removeRecursively() && pathEntryExists(stagingPath)) {
                finalMessage.append(QStringLiteral(" The import staging directory could not be removed."));
            }
            stagingPath.clear();
        }
        m_running.store(false);
        emit finished(success, finalMessage, rootPath, count);
    };
    const auto cancelled = [this]() { return m_cancelled.load(); };
    const auto cancellationFailed = [&]() {
        if (!cancelled())
            return false;
        complete(false, QStringLiteral("Remote import was cancelled."), {}, 0);
        return true;
    };

    if (m_localRootPath.trimmed().isEmpty()) {
        complete(false, QStringLiteral("The local notebook path must not be empty."), {}, 0);
        return;
    }
    targetPath = QDir::cleanPath(QDir(m_localRootPath).absolutePath());
    if (pathEntryExists(targetPath)) {
        complete(false, QStringLiteral("The local notebook target already exists."), {}, 0);
        return;
    }
    if (!validUuid(m_remoteNotebookId)) {
        complete(false, QStringLiteral("The remote notebook ID is invalid."), {}, 0);
        return;
    }
    if (m_masterPassword.isEmpty()) {
        complete(false, QStringLiteral("The master password must not be empty."), {}, 0);
        return;
    }

    SyncSettings remoteSettings = m_settings;
    remoteSettings.remotePath = m_settings.remotePath + QStringLiteral("/") + m_remoteNotebookId;
    m_client->configure(remoteSettings, m_serverPassword);

    QString error;
    if (!m_client->isConfigured(&error)) {
        complete(false, error.isEmpty() ? QStringLiteral("The WebDAV configuration is invalid.") : error,
                 {}, 0);
        return;
    }
    if (!CryptoService::isAvailable(&error)) {
        complete(false, QStringLiteral("Encrypted remote import is unavailable: %1").arg(error), {}, 0);
        return;
    }
    if (cancellationFailed())
        return;

    emit progress(QStringLiteral("Downloading the remote notebook metadata."));
    QScopedPointer<CryptoService> crypto(new CryptoService);
    const WebDavResponse headerResponse = m_client->get(QStringLiteral("header.json"));
    if (!successful(headerResponse, QStringLiteral("Downloading the encryption header"), &error)) {
        if (cancellationFailed())
            return;
        complete(false, error, {}, 0);
        return;
    }

    QJsonObject header;
    if (!parseObject(headerResponse.body, &header, QStringLiteral("The remote encryption header"), &error)
        || !crypto->initializeFromHeader(header, m_masterPassword, &error)) {
        complete(false, error, {}, 0);
        return;
    }
    if (cancellationFailed())
        return;

    const WebDavResponse manifestResponse = m_client->get(QStringLiteral("manifest.bin"));
    if (!successful(manifestResponse, QStringLiteral("Downloading the manifest"), &error)) {
        if (cancellationFailed())
            return;
        complete(false, error, {}, 0);
        return;
    }

    QByteArray manifestPlainText;
    QJsonObject manifest;
    QHash<QString, RemoteEntry> remoteEntries;
    QHash<QString, RemoteEntry> remoteAssetEntries;
    QString manifestNotebookName;
    if (!crypto->decrypt(manifestResponse.body, kManifestContext, &manifestPlainText, &error)
        || !parseObject(manifestPlainText, &manifest, QStringLiteral("The remote manifest"), &error)
        || !parseManifest(manifest, &remoteEntries, &remoteAssetEntries, &manifestNotebookName, &error)) {
        complete(false, error, {}, 0);
        return;
    }

    QStringList ids = remoteEntries.keys();
    std::sort(ids.begin(), ids.end());
    QHash<QString, PendingImport> pendingImports;
    QHash<QString, QString> manifestIdsByNormalizedId;
    for (const QString &id : ids)
        manifestIdsByNormalizedId.insert(normalizedUuid(id), id);

    int downloadedObjects = 0;
    for (const QString &id : ids) {
        if (cancellationFailed())
            return;

        const WebDavResponse objectResponse = m_client->get(objectPath(id));
        if (!successful(objectResponse, QStringLiteral("Downloading a remote object"), &error)) {
            if (cancellationFailed())
                return;
            complete(false, error, {}, 0);
            return;
        }

        QByteArray plainText;
        QJsonObject payload;
        if (!crypto->decrypt(objectResponse.body, objectContext(id), &plainText, &error)
            || !parseObject(plainText, &payload, QStringLiteral("A remote object"), &error)
            || payload.value(QStringLiteral("version")).toInt(-1) != 1
            || !payload.value(QStringLiteral("node")).isObject()
            || !payload.value(QStringLiteral("content")).isString()) {
            complete(false, error.isEmpty()
                             ? QStringLiteral("A remote object has an invalid payload.") : error,
                     {}, 0);
            return;
        }

        PendingImport pending;
        pending.content = payload.value(QStringLiteral("content")).toString();
        if (!nodeFromJson(payload.value(QStringLiteral("node")).toObject(), id,
                          &pending.node, &error)) {
            complete(false, error, {}, 0);
            return;
        }
        const RemoteEntry entry = remoteEntries.value(id);
        if (pending.node.updatedAt != entry.updatedAt || pending.node.writerId != entry.writerId) {
            complete(false, QStringLiteral("A remote object does not match its manifest entry."), {}, 0);
            return;
        }
        pendingImports.insert(id, pending);
        ++downloadedObjects;
        emit progress(QStringLiteral("Downloaded %1 of %2 remote object(s).")
                          .arg(downloadedObjects).arg(ids.size()));
    }

    if (cancellationFailed())
        return;
    if (pathEntryExists(targetPath)) {
        complete(false, QStringLiteral("The local notebook target already exists."), {}, 0);
        return;
    }

    const QFileInfo targetInfo(targetPath);
    const QString parentPath = targetInfo.absolutePath();
    QDir parentDirectory(parentPath);
    if (!QDir().mkpath(parentPath)) {
        complete(false, QStringLiteral("Cannot create the local notebook parent directory."), {}, 0);
        return;
    }
    for (int attempt = 0; attempt < 32 && stagingPath.isEmpty(); ++attempt) {
        const QString stagingName = QStringLiteral(".qoplin-import-%1").arg(
            QUuid::createUuid().toString(QUuid::WithoutBraces));
        const QString candidate = parentDirectory.filePath(stagingName);
        if (parentDirectory.mkdir(stagingName)) {
            stagingPath = candidate;
        } else if (!pathEntryExists(candidate)) {
            complete(false, QStringLiteral("Cannot create the import staging directory."), {}, 0);
            return;
        }
    }
    if (stagingPath.isEmpty()) {
        complete(false, QStringLiteral("Cannot create a unique import staging directory."), {}, 0);
        return;
    }

    QString notebookName = m_requestedLocalName;
    if (notebookName.isEmpty())
        notebookName = manifestNotebookName;
    if (notebookName.isEmpty())
        notebookName = QStringLiteral("Imported notebook");

    emit progress(QStringLiteral("Creating the local notebook."));
    NotebookStore store;
    if (!store.createWithId(stagingPath, notebookName, m_remoteNotebookId, m_writerId, m_masterPassword, &error)) {
        complete(false, error, {}, 0);
        return;
    }

    // Resolve parents case-insensitively, as NotebookStore normalizes UUIDs.
    QHash<QString, int> applyState;
    std::function<bool(const QString &)> applyImport = [&](const QString &id) -> bool {
        if (cancelled())
            return fail(&error, QStringLiteral("Remote import was cancelled."));
        if (applyState.value(id) == 2)
            return true;
        if (applyState.value(id) == 1)
            return fail(&error, QStringLiteral("The remote node hierarchy contains a cycle."));
        applyState.insert(id, 1);

        const PendingImport pending = pendingImports.value(id);
        if (!pending.node.parentId.isEmpty()) {
            const QString parentId = manifestIdsByNormalizedId.value(
                normalizedUuid(pending.node.parentId));
            if (parentId.isEmpty())
                return fail(&error, QStringLiteral("The remote node hierarchy contains a missing parent."));
            if (!applyImport(parentId))
                return false;
        }
        if (!store.applyRemoteNode(pending.node, pending.content, &error))
            return false;
        applyState.insert(id, 2);
        ++importedObjects;
        emit progress(QStringLiteral("Imported %1 object(s).").arg(importedObjects));
        return true;
    };

    for (const QString &id : ids) {
        if (!applyImport(id)) {
            complete(false, error, {}, 0);
            return;
        }
    }

    QStringList assetIds = remoteAssetEntries.keys();
    std::sort(assetIds.begin(), assetIds.end());
    // Import and release one asset at a time. Older remotes may list no assets, and
    // an asset listed by an older/incomplete remote is allowed to be absent.
    for (const QString &id : assetIds) {
        if (cancellationFailed())
            return;
        const WebDavResponse assetResponse = m_client->get(remoteAssetPath(id));
        if (assetResponse.statusCode == 404)
            continue;
        if (!successful(assetResponse, QStringLiteral("Downloading a remote asset"), &error)) {
            if (cancellationFailed())
                return;
            complete(false, error, {}, 0);
            return;
        }

        QByteArray plainText;
        AssetRecord asset;
        QByteArray assetData;
        if (!crypto->decrypt(assetResponse.body, assetContext(id), &plainText, &error)
            || !decodeAssetEnvelope(plainText, id, &asset, &assetData, &error)) {
            complete(false, error, {}, 0);
            return;
        }
        const RemoteEntry entry = remoteAssetEntries.value(id);
        if (asset.updatedAt != entry.updatedAt || asset.writerId != entry.writerId) {
            complete(false, QStringLiteral("A remote asset does not match its manifest entry."), {}, 0);
            return;
        }
        if (!store.applyRemoteAsset(asset, assetData, &error)) {
            complete(false, error, {}, 0);
            return;
        }
        ++importedObjects;
        emit progress(QStringLiteral("Imported %1 object(s).").arg(importedObjects));
    }
    if (cancellationFailed())
        return;
    if (pathEntryExists(targetPath)) {
        complete(false, QStringLiteral("The local notebook target already exists."), {}, 0);
        return;
    }

    // The staging directory is a sibling, so this rename stays on one file system.
    if (!QDir().rename(stagingPath, targetPath)) {
        complete(false, QStringLiteral("Cannot move the imported notebook to its target path."), {}, 0);
        return;
    }
    stagingPath.clear();
    complete(true, QStringLiteral("Notebook imported successfully: %1 object(s).").arg(importedObjects),
             targetPath, importedObjects);
}
