#include "SyncWorker.h"

#include "CryptoService.h"
#include "WebDavClient.h"
#include "models/NoteNode.h"
#include "storage/NotebookStore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QScopedPointer>
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

struct PendingDownload
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
    // Integer doubles above this range cannot represent every qint64 exactly.
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

QJsonObject nodeToJson(const NoteNode &node)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), node.id);
    object.insert(QStringLiteral("parentId"), node.parentId);
    object.insert(QStringLiteral("type"), node.isFolder() ? QStringLiteral("folder")
                                                          : QStringLiteral("note"));
    object.insert(QStringLiteral("title"), node.title);
    object.insert(QStringLiteral("fileName"), node.fileName);
    object.insert(QStringLiteral("createdAt"), QString::number(node.createdAt));
    object.insert(QStringLiteral("updatedAt"), QString::number(node.updatedAt));
    object.insert(QStringLiteral("writerId"), node.writerId);
    object.insert(QStringLiteral("sortOrder"), node.sortOrder);
    object.insert(QStringLiteral("deleted"), node.deleted);
    return object;
}

QJsonObject assetToJson(const AssetRecord &asset)
{
    return {
        {QStringLiteral("id"), asset.id},
        {QStringLiteral("mimeType"), asset.mimeType},
        {QStringLiteral("extension"), asset.extension},
        {QStringLiteral("originalName"), asset.originalName},
        {QStringLiteral("size"), QString::number(asset.size)},
        {QStringLiteral("sha256"), asset.sha256},
        {QStringLiteral("createdAt"), QString::number(asset.createdAt)},
        {QStringLiteral("updatedAt"), QString::number(asset.updatedAt)},
        {QStringLiteral("writerId"), asset.writerId},
        {QStringLiteral("deleted"), asset.deleted}
    };
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

QByteArray encodeAssetEnvelope(const AssetRecord &asset, const QByteArray &data)
{
    const QByteArray metadata = QJsonDocument(assetToJson(asset)).toJson(QJsonDocument::Compact);
    const quint32 length = static_cast<quint32>(metadata.size());
    QByteArray envelope;
    envelope.reserve(4 + metadata.size() + data.size());
    envelope.append(static_cast<char>((length >> 24) & 0xff));
    envelope.append(static_cast<char>((length >> 16) & 0xff));
    envelope.append(static_cast<char>((length >> 8) & 0xff));
    envelope.append(static_cast<char>(length & 0xff));
    envelope.append(metadata);
    envelope.append(data);
    return envelope;
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

bool parseManifestEntries(const QJsonArray &array, QHash<QString, RemoteEntry> *entries,
                          QString *error)
{
    entries->clear();
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
            || entry.updatedAt < 0 || entries->contains(entry.id)) {
            return fail(error, QStringLiteral("The remote manifest contains an unsafe, invalid, or duplicate entry."));
        }
        entries->insert(entry.id, entry);
    }
    return true;
}

bool parseManifest(const QJsonObject &object, QHash<QString, RemoteEntry> *entries,
                   QHash<QString, RemoteEntry> *assetEntries, QString *error)
{
    if (object.value(QStringLiteral("version")).toInt(-1) != kManifestVersion
        || !object.value(QStringLiteral("nodes")).isArray()) {
        return fail(error, QStringLiteral("The remote manifest has an unsupported format."));
    }
    const QJsonValue assets = object.value(QStringLiteral("assets"));
    if (!assets.isUndefined() && !assets.isArray())
        return fail(error, QStringLiteral("The remote manifest has an invalid asset list."));
    return parseManifestEntries(object.value(QStringLiteral("nodes")).toArray(), entries, error)
        && parseManifestEntries(assets.toArray(), assetEntries, error);
}

QJsonArray manifestEntriesToJson(const QHash<QString, RemoteEntry> &entries)
{
    QStringList ids = entries.keys();
    std::sort(ids.begin(), ids.end());
    QJsonArray result;
    for (const QString &id : ids) {
        const RemoteEntry &entry = entries[id];
        QJsonObject object;
        object.insert(QStringLiteral("id"), entry.id);
        object.insert(QStringLiteral("updatedAt"), QString::number(entry.updatedAt));
        object.insert(QStringLiteral("writerId"), entry.writerId);
        result.append(object);
    }
    return result;
}

QByteArray manifestToJson(const QHash<QString, RemoteEntry> &entries,
                          const QHash<QString, RemoteEntry> &assetEntries,
                          const QString &notebookName)
{
    QJsonObject manifest;
    manifest.insert(QStringLiteral("version"), kManifestVersion);
    if (!notebookName.isEmpty()) manifest.insert(QStringLiteral("notebookName"), notebookName);
    manifest.insert(QStringLiteral("nodes"), manifestEntriesToJson(entries));
    manifest.insert(QStringLiteral("assets"), manifestEntriesToJson(assetEntries));
    return QJsonDocument(manifest).toJson(QJsonDocument::Compact);
}

enum class Comparison { LocalNewer, RemoteNewer, Equal };

Comparison compareRevision(qint64 localUpdatedAt, const QString &localWriterId,
                           const RemoteEntry &remote)
{
    if (localUpdatedAt > remote.updatedAt)
        return Comparison::LocalNewer;
    if (localUpdatedAt < remote.updatedAt)
        return Comparison::RemoteNewer;
    const int writerComparison = QString::compare(localWriterId, remote.writerId, Qt::CaseSensitive);
    if (writerComparison > 0)
        return Comparison::LocalNewer;
    if (writerComparison < 0)
        return Comparison::RemoteNewer;
    return Comparison::Equal;
}

Comparison compareNodes(const NoteNode &local, const RemoteEntry &remote)
{
    return compareRevision(local.updatedAt, local.writerId, remote);
}

Comparison compareAssets(const AssetRecord &local, const RemoteEntry &remote)
{
    return compareRevision(local.updatedAt, local.writerId, remote);
}

} // namespace

SyncWorker::SyncWorker(NotebookStore *store, QObject *parent)
    : QObject(parent)
    , m_store(store)
    , m_client(new WebDavClient(this))
{
}

SyncWorker::~SyncWorker()
{
    m_serverPassword.fill(QChar::Null);
    m_masterPassword.fill(QChar::Null);
    m_serverPassword.clear();
    m_masterPassword.clear();
}

void SyncWorker::configure(const SyncSettings &settings, const QString &serverPassword,
                           const QString &masterPassword)
{
    if (m_running.load())
        return;
    m_settings = settings;
    m_serverPassword = serverPassword;
    m_masterPassword = masterPassword;
}

void SyncWorker::setRequestTimeout(int milliseconds)
{
    m_client->setTimeout(milliseconds);
}

void SyncWorker::cancel()
{
    m_cancelled.store(true);
    m_client->cancel();
}

void SyncWorker::start()
{
    if (m_running.exchange(true)) {
        emit finished(false, QStringLiteral("Synchronization is already running."), 0, 0);
        return;
    }

    int uploaded = 0;
    int downloaded = 0;
    m_cancelled.store(false);
    m_client->resetCancellation();

    const auto complete = [this](bool success, const QString &message, int sent, int received) {
        m_running.store(false);
        emit finished(success, message, sent, received);
    };
    const auto cancelled = [this]() { return m_cancelled.load(); };

    if (!m_store || !m_store->isOpen()) {
        complete(false, QStringLiteral("No notebook is open."), uploaded, downloaded);
        return;
    }
    SyncSettings remoteSettings = m_settings;
    remoteSettings.remotePath = m_settings.remotePath + QStringLiteral("/") + m_store->id();
    m_client->configure(remoteSettings, m_serverPassword);
    if (m_masterPassword.isEmpty()) {
        complete(false, QStringLiteral("The master password must not be empty."), uploaded, downloaded);
        return;
    }

    QString error;
    if (!m_client->isConfigured(&error)) {
        complete(false, error.isEmpty() ? QStringLiteral("The WebDAV configuration is invalid.") : error,
                 uploaded, downloaded);
        return;
    }
    if (!CryptoService::isAvailable(&error)) {
        complete(false, QStringLiteral("Encrypted synchronization is unavailable: %1").arg(error),
                 uploaded, downloaded);
        return;
    }

    emit progress(QStringLiteral("Preparing the remote collection."));
    if (!m_client->ensureCollection(QString(), &error)
        || !m_client->ensureCollection(QStringLiteral("objects"), &error)
        || !m_client->ensureCollection(QStringLiteral("assets"), &error)) {
        complete(false, error, uploaded, downloaded);
        return;
    }

    QScopedPointer<CryptoService> crypto(new CryptoService);
    const WebDavResponse headerResponse = m_client->get(QStringLiteral("header.json"));
    if (headerResponse.statusCode == 404) {
        emit progress(QStringLiteral("Initializing encrypted remote storage."));
        QJsonObject header;
        if (!crypto->createHeader(m_masterPassword, &header, &error)) {
            complete(false, error, uploaded, downloaded);
            return;
        }
        const WebDavResponse putResponse = m_client->put(
            QStringLiteral("header.json"), QJsonDocument(header).toJson(QJsonDocument::Compact));
        if (!successful(putResponse, QStringLiteral("Uploading the encryption header"), &error)) {
            complete(false, error, uploaded, downloaded);
            return;
        }
    } else if (headerResponse.isSuccess()) {
        QJsonObject header;
        if (!parseObject(headerResponse.body, &header, QStringLiteral("The remote encryption header"), &error)
            || !crypto->initializeFromHeader(header, m_masterPassword, &error)) {
            complete(false, error, uploaded, downloaded);
            return;
        }
    } else {
        complete(false, responseFailure(QStringLiteral("Downloading the encryption header"), headerResponse),
                 uploaded, downloaded);
        return;
    }

    QHash<QString, RemoteEntry> remoteEntries;
    QHash<QString, RemoteEntry> remoteAssetEntries;
    bool manifestChanged = false;
    const WebDavResponse manifestResponse = m_client->get(QStringLiteral("manifest.bin"));
    if (manifestResponse.statusCode == 404) {
        manifestChanged = true;
    } else if (manifestResponse.isSuccess()) {
        QByteArray plainText;
        QJsonObject manifest;
        if (!crypto->decrypt(manifestResponse.body, kManifestContext, &plainText, &error)
            || !parseObject(plainText, &manifest, QStringLiteral("The remote manifest"), &error)
            || !parseManifest(manifest, &remoteEntries, &remoteAssetEntries, &error)) {
            complete(false, error, uploaded, downloaded);
            return;
        }
        if (manifest.value(QStringLiteral("notebookName")).toString().isEmpty())
            manifestChanged = true;
    } else {
        complete(false, responseFailure(QStringLiteral("Downloading the manifest"), manifestResponse),
                 uploaded, downloaded);
        return;
    }

    QHash<QString, NoteNode> localNodes;
    const QVector<NoteNode> allLocalNodes = m_store->nodes(true);
    for (const NoteNode &node : allLocalNodes)
        localNodes.insert(node.id, node);

    QStringList ids = localNodes.keys();
    ids.append(remoteEntries.keys());
    ids.removeDuplicates();
    std::sort(ids.begin(), ids.end());
    QHash<QString, PendingDownload> pendingDownloads;

    for (const QString &id : ids) {
        if (cancelled()) {
            complete(false, QStringLiteral("Synchronization was cancelled."), uploaded, downloaded);
            return;
        }

        const bool hasLocal = localNodes.contains(id);
        const bool hasRemote = remoteEntries.contains(id);
        if (hasLocal && hasRemote
            && compareNodes(localNodes.value(id), remoteEntries.value(id)) == Comparison::Equal) {
            continue;
        }

        if (hasRemote && (!hasLocal
                          || compareNodes(localNodes.value(id), remoteEntries.value(id))
                              == Comparison::RemoteNewer)) {
            const WebDavResponse objectResponse = m_client->get(objectPath(id));
            if (!successful(objectResponse, QStringLiteral("Downloading a remote object"), &error)) {
                complete(false, error, uploaded, downloaded);
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
                         uploaded, downloaded);
                return;
            }

            PendingDownload pending;
            pending.content = payload.value(QStringLiteral("content")).toString();
            if (!nodeFromJson(payload.value(QStringLiteral("node")).toObject(), id,
                              &pending.node, &error)) {
                complete(false, error, uploaded, downloaded);
                return;
            }
            if (pending.node.updatedAt != remoteEntries.value(id).updatedAt
                || pending.node.writerId != remoteEntries.value(id).writerId) {
                complete(false, QStringLiteral("A remote object does not match its manifest entry."),
                         uploaded, downloaded);
                return;
            }
            pendingDownloads.insert(id, pending);
            continue;
        }

        if (!hasLocal) {
            complete(false, QStringLiteral("The local node index is inconsistent."), uploaded, downloaded);
            return;
        }
        const NoteNode node = localNodes.value(id);
        if (!validUuid(node.id)) {
            complete(false, QStringLiteral("A local node has an invalid UUID."), uploaded, downloaded);
            return;
        }
        QString content;
        if (node.isNote() && !node.deleted) {
            error.clear();
            content = m_store->readNote(node.id, &error);
            if (!error.isEmpty()) {
                complete(false, error, uploaded, downloaded);
                return;
            }
        }

        QJsonObject payload;
        payload.insert(QStringLiteral("version"), 1);
        payload.insert(QStringLiteral("node"), nodeToJson(node));
        payload.insert(QStringLiteral("content"), content);
        QByteArray encrypted;
        if (!crypto->encrypt(QJsonDocument(payload).toJson(QJsonDocument::Compact),
                             objectContext(id), &encrypted, &error)) {
            complete(false, error, uploaded, downloaded);
            return;
        }
        const WebDavResponse putResponse = m_client->put(objectPath(id), encrypted);
        if (!successful(putResponse, QStringLiteral("Uploading a local object"), &error)) {
            complete(false, error, uploaded, downloaded);
            return;
        }
        RemoteEntry entry;
        entry.id = node.id;
        entry.updatedAt = node.updatedAt;
        entry.writerId = node.writerId;
        remoteEntries.insert(id, entry);
        manifestChanged = true;
        ++uploaded;
        emit progress(QStringLiteral("Uploaded %1 object(s).").arg(uploaded));
    }

    // Apply parents before children when both were downloaded in this pass.
    QHash<QString, int> applyState;
    std::function<bool(const QString &)> applyDownload = [&](const QString &id) -> bool {
        if (applyState.value(id) == 2)
            return true;
        if (applyState.value(id) == 1)
            return fail(&error, QStringLiteral("The remote node hierarchy contains a cycle."));
        applyState.insert(id, 1);
        const PendingDownload pending = pendingDownloads.value(id);
        if (!pending.node.parentId.isEmpty() && pendingDownloads.contains(pending.node.parentId)
            && !applyDownload(pending.node.parentId)) {
            return false;
        }
        if (!m_store->applyRemoteNode(pending.node, pending.content, &error))
            return false;
        applyState.insert(id, 2);
        ++downloaded;
        emit progress(QStringLiteral("Downloaded %1 object(s).").arg(downloaded));
        return true;
    };
    for (auto it = pendingDownloads.constBegin(); it != pendingDownloads.constEnd(); ++it) {
        if (cancelled()) {
            complete(false, QStringLiteral("Synchronization was cancelled."), uploaded, downloaded);
            return;
        }
        if (!applyDownload(it.key())) {
            complete(false, error, uploaded, downloaded);
            return;
        }
    }

    QHash<QString, AssetRecord> localAssets;
    const QVector<AssetRecord> allLocalAssets = m_store->assets(true);
    for (const AssetRecord &asset : allLocalAssets)
        localAssets.insert(asset.id, asset);

    QStringList assetIds = localAssets.keys();
    assetIds.append(remoteAssetEntries.keys());
    assetIds.removeDuplicates();
    std::sort(assetIds.begin(), assetIds.end());

    // Assets have no hierarchy, so each download can be applied and released before
    // the next one is fetched. At most one asset payload is held by this loop.
    for (const QString &id : assetIds) {
        if (cancelled()) {
            complete(false, QStringLiteral("Synchronization was cancelled."), uploaded, downloaded);
            return;
        }

        const bool hasLocal = localAssets.contains(id);
        const bool hasRemote = remoteAssetEntries.contains(id);
        if (hasLocal && hasRemote
            && compareAssets(localAssets.value(id), remoteAssetEntries.value(id)) == Comparison::Equal) {
            continue;
        }

        if (hasRemote && (!hasLocal
                          || compareAssets(localAssets.value(id), remoteAssetEntries.value(id))
                              == Comparison::RemoteNewer)) {
            const WebDavResponse assetResponse = m_client->get(remoteAssetPath(id));
            if (!successful(assetResponse, QStringLiteral("Downloading a remote asset"), &error)) {
                complete(false, error, uploaded, downloaded);
                return;
            }
            QByteArray plainText;
            AssetRecord remoteAsset;
            QByteArray assetData;
            if (!crypto->decrypt(assetResponse.body, assetContext(id), &plainText, &error)
                || !decodeAssetEnvelope(plainText, id, &remoteAsset, &assetData, &error)) {
                complete(false, error, uploaded, downloaded);
                return;
            }
            const RemoteEntry entry = remoteAssetEntries.value(id);
            if (remoteAsset.updatedAt != entry.updatedAt || remoteAsset.writerId != entry.writerId) {
                complete(false, QStringLiteral("A remote asset does not match its manifest entry."),
                         uploaded, downloaded);
                return;
            }
            if (!m_store->applyRemoteAsset(remoteAsset, assetData, &error)) {
                complete(false, error, uploaded, downloaded);
                return;
            }
            ++downloaded;
            emit progress(QStringLiteral("Downloaded %1 object(s).").arg(downloaded));
            continue;
        }

        if (!hasLocal) {
            complete(false, QStringLiteral("The local asset index is inconsistent."), uploaded, downloaded);
            return;
        }
        const AssetRecord asset = localAssets.value(id);
        if (!validUuid(asset.id)) {
            complete(false, QStringLiteral("A local asset has an invalid UUID."), uploaded, downloaded);
            return;
        }
        QByteArray assetData;
        if (!asset.deleted) {
            error.clear();
            assetData = m_store->readAsset(asset.id, &error);
            if (!error.isEmpty()) {
                complete(false, error, uploaded, downloaded);
                return;
            }
        }

        QByteArray encrypted;
        if (!crypto->encrypt(encodeAssetEnvelope(asset, assetData), assetContext(id),
                             &encrypted, &error)) {
            complete(false, error, uploaded, downloaded);
            return;
        }
        const WebDavResponse putResponse = m_client->put(remoteAssetPath(id), encrypted);
        if (!successful(putResponse, QStringLiteral("Uploading a local asset"), &error)) {
            complete(false, error, uploaded, downloaded);
            return;
        }
        RemoteEntry entry;
        entry.id = asset.id;
        entry.updatedAt = asset.updatedAt;
        entry.writerId = asset.writerId;
        remoteAssetEntries.insert(id, entry);
        manifestChanged = true;
        ++uploaded;
        emit progress(QStringLiteral("Uploaded %1 object(s).").arg(uploaded));
    }

    if (manifestChanged) {
        if (cancelled()) {
            complete(false, QStringLiteral("Synchronization was cancelled."), uploaded, downloaded);
            return;
        }
        emit progress(QStringLiteral("Updating the remote manifest."));
        QByteArray encryptedManifest;
        if (!crypto->encrypt(manifestToJson(remoteEntries, remoteAssetEntries, m_store->name()),
                             kManifestContext, &encryptedManifest, &error)) {
            complete(false, error, uploaded, downloaded);
            return;
        }
        const WebDavResponse putResponse = m_client->put(QStringLiteral("manifest.bin"), encryptedManifest);
        if (!successful(putResponse, QStringLiteral("Uploading the manifest"), &error)) {
            complete(false, error, uploaded, downloaded);
            return;
        }
    }

    complete(true, QStringLiteral("Synchronization completed: %1 uploaded, %2 downloaded.")
                       .arg(uploaded).arg(downloaded),
             uploaded, downloaded);
}
