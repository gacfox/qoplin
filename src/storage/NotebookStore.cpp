#include "storage/NotebookStore.h"

#include "sync/CryptoService.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
constexpr int kSchemaVersion = 1;

const QString kHeaderName = QStringLiteral("qoplin.json");
const QString kIndexName = QStringLiteral(".qoplin/index.bin");
const QString kNotesDirectory = QStringLiteral(".qoplin/notes");
const QString kAssetsDirectory = QStringLiteral(".qoplin/assets");
const QString kInternalDirectory = QStringLiteral(".qoplin");
const QString kBackupSuffix = QStringLiteral(".bak");
const QString kNoteSuffix = QStringLiteral(".bin");
// Index file of the old plaintext notebooks; it marks an unsupported notebook.
const QString kLegacyIndexName = QStringLiteral(".qoplin/index.json");

// The associated data binds every ciphertext to the local format, its notebook, and its object.
QByteArray indexAssociatedData(const QString &notebookId)
{
    return QByteArrayLiteral("qoplin/local/index/v1:") + notebookId.toUtf8();
}

QByteArray noteAssociatedData(const QString &notebookId, const QString &noteId)
{
    return QByteArrayLiteral("qoplin/local/note/v1:") + notebookId.toUtf8() + ':' + noteId.toUtf8();
}

QByteArray assetAssociatedData(const QString &notebookId, const QString &assetId)
{
    return QByteArrayLiteral("qoplin/local/asset/v1:") + notebookId.toUtf8() + ':' + assetId.toUtf8();
}

QString legacyNotebookMessage()
{
    return QStringLiteral("This notebook uses the old unencrypted format, which is no longer "
                          "supported. It cannot be converted; create a new encrypted notebook instead.");
}

QString normalizedUuid(const QString &value)
{
    const QUuid uuid(value);
    return uuid.isNull() ? QString() : uuid.toString(QUuid::WithoutBraces).toLower();
}

bool validTitle(const QString &title, QString *reason)
{
    auto fail = [reason](const QString &message) {
        if (reason)
            *reason = message;
        return false;
    };

    if (title.isEmpty() || title.trimmed().isEmpty())
        return fail(QStringLiteral("The title must not be empty."));
    if (title.size() > 255)
        return fail(QStringLiteral("The title is too long."));
    if (title != title.trimmed() || title.endsWith(QLatin1Char('.')))
        return fail(QStringLiteral("The title has invalid leading or trailing characters."));
    if (title == QStringLiteral(".") || title == QStringLiteral(".."))
        return fail(QStringLiteral("The title is reserved."));

    static const QString invalidCharacters = QStringLiteral("<>:\"/\\|?*");
    for (const QChar ch : title) {
        if (ch.isNull() || ch.unicode() < 32 || ch.unicode() == 127
            || invalidCharacters.contains(ch))
            return fail(QStringLiteral("The title contains a character that is not allowed."));
    }

    const QString baseName = title.section(QLatin1Char('.'), 0, 0).toUpper();
    static const QSet<QString> reservedNames = {
        QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"),
        QStringLiteral("NUL"), QStringLiteral("COM1"), QStringLiteral("COM2"),
        QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("COM5"),
        QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"),
        QStringLiteral("LPT3"), QStringLiteral("LPT4"), QStringLiteral("LPT5"),
        QStringLiteral("LPT6"), QStringLiteral("LPT7"), QStringLiteral("LPT8"),
        QStringLiteral("LPT9")
    };
    if (reservedNames.contains(baseName))
        return fail(QStringLiteral("The title is reserved by the file system."));

    return true;
}

// Writes data through QSaveFile, so a crash never leaves a partially written object.
bool writeAtomically(const QString &path, const QByteArray &data, QString *error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("Cannot write %1: %2").arg(path, file.errorString());
        return false;
    }
    if (file.write(data) != data.size()) {
        if (error)
            *error = QStringLiteral("Cannot write %1: %2").arg(path, file.errorString());
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        if (error)
            *error = QStringLiteral("Cannot commit %1: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}

// Stores an object and an identical backup. A backup failure is not fatal: the
// primary copy is already committed.
bool writeWithBackup(const QString &path, const QByteArray &data, QString *error)
{
    if (!writeAtomically(path, data, error))
        return false;
    QString backupError;
    writeAtomically(path + kBackupSuffix, data, &backupError);
    return true;
}

bool readFileBytes(const QString &path, QByteArray *data, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("Cannot read %1: %2").arg(path, file.errorString());
        return false;
    }
    *data = file.readAll();
    return true;
}

bool parseJsonObject(const QByteArray &data, const QString &path, QJsonObject *object, QString *error)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) {
            *error = QStringLiteral("Invalid JSON in %1: %2")
                         .arg(path, parseError.errorString());
        }
        return false;
    }
    *object = document.object();
    return true;
}

bool jsonInt64(const QJsonObject &object, const QString &key, qint64 defaultValue,
               qint64 *value, QString *error)
{
    const auto it = object.find(key);
    if (it == object.end()) {
        *value = defaultValue;
        return true;
    }
    if (!it->isDouble()) {
        if (error)
            *error = QStringLiteral("Field '%1' must be an integer.").arg(key);
        return false;
    }
    const double number = it->toDouble();
    if (!std::isfinite(number) || number < 0
        || number > static_cast<double>(std::numeric_limits<qint64>::max())
        || std::floor(number) != number) {
        if (error)
            *error = QStringLiteral("Field '%1' is outside the valid integer range.").arg(key);
        return false;
    }
    *value = static_cast<qint64>(number);
    return true;
}

QJsonObject nodeToJson(const NoteNode &node)
{
    return {
        {QStringLiteral("id"), node.id},
        {QStringLiteral("parentId"), node.parentId},
        {QStringLiteral("type"), node.isFolder() ? QStringLiteral("folder")
                                                 : QStringLiteral("note")},
        {QStringLiteral("title"), node.title},
        {QStringLiteral("fileName"), node.fileName},
        {QStringLiteral("createdAt"), static_cast<double>(node.createdAt)},
        {QStringLiteral("updatedAt"), static_cast<double>(node.updatedAt)},
        {QStringLiteral("writerId"), node.writerId},
        {QStringLiteral("sortOrder"), node.sortOrder},
        {QStringLiteral("deleted"), node.deleted}
    };
}

QJsonObject assetToJson(const AssetRecord &asset)
{
    return {
        {QStringLiteral("id"), asset.id},
        {QStringLiteral("mimeType"), asset.mimeType},
        {QStringLiteral("extension"), asset.extension},
        {QStringLiteral("originalName"), asset.originalName},
        {QStringLiteral("size"), static_cast<double>(asset.size)},
        {QStringLiteral("sha256"), asset.sha256},
        {QStringLiteral("createdAt"), static_cast<double>(asset.createdAt)},
        {QStringLiteral("updatedAt"), static_cast<double>(asset.updatedAt)},
        {QStringLiteral("writerId"), asset.writerId},
        {QStringLiteral("deleted"), asset.deleted}
    };
}

bool normalizeAsset(const AssetRecord &input, AssetRecord *output, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

    AssetRecord result = input;
    result.id = normalizedUuid(input.id);
    if (result.id.isEmpty())
        return fail(QStringLiteral("Every asset must have a valid UUID."));
    if (result.createdAt < 0 || result.updatedAt < 0 || result.size < 0)
        return fail(QStringLiteral("Asset %1 has a negative timestamp or size.").arg(result.id));

    QString extension = result.extension;
    if (extension.startsWith(QLatin1Char('.')))
        extension.remove(0, 1);
    extension = extension.toLower();
    static const QSet<QString> supportedExtensions = {
        QStringLiteral("apng"), QStringLiteral("avif"), QStringLiteral("bmp"),
        QStringLiteral("gif"), QStringLiteral("heic"), QStringLiteral("heif"),
        QStringLiteral("ico"), QStringLiteral("jfif"), QStringLiteral("jpeg"),
        QStringLiteral("jpg"), QStringLiteral("png"), QStringLiteral("svg"),
        QStringLiteral("tif"), QStringLiteral("tiff"), QStringLiteral("webp")
    };
    if (!supportedExtensions.contains(extension))
        return fail(QStringLiteral("Asset %1 has an invalid image extension.").arg(result.id));
    if (result.originalName == QStringLiteral(".") || result.originalName == QStringLiteral("..")
        || result.originalName.contains(QLatin1Char('/'))
        || result.originalName.contains(QLatin1Char('\\'))
        || result.originalName.contains(QChar::Null)) {
        return fail(QStringLiteral("Asset %1 has an unsafe original file name.").arg(result.id));
    }
    if (!result.sha256.isEmpty()) {
        if (result.sha256.size() != 64)
            return fail(QStringLiteral("Asset %1 has an invalid SHA-256 digest.").arg(result.id));
        for (const QChar ch : result.sha256) {
            const ushort value = ch.unicode();
            const bool hexadecimal = (value >= '0' && value <= '9')
                || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F');
            if (!hexadecimal)
                return fail(QStringLiteral("Asset %1 has an invalid SHA-256 digest.").arg(result.id));
        }
    }

    *output = result;
    return true;
}

bool normalizeNode(const NoteNode &input, NoteNode *output, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

    NoteNode result = input;
    result.id = normalizedUuid(input.id);
    if (result.id.isEmpty())
        return fail(QStringLiteral("Every node must have a valid UUID."));
    if (!input.parentId.isEmpty()) {
        result.parentId = normalizedUuid(input.parentId);
        if (result.parentId.isEmpty())
            return fail(QStringLiteral("Node %1 has an invalid parent UUID.").arg(result.id));
        if (result.parentId == result.id)
            return fail(QStringLiteral("A node cannot be its own parent."));
    } else {
        result.parentId.clear();
    }

    QString titleError;
    if (!validTitle(input.title, &titleError))
        return fail(QStringLiteral("Invalid title for node %1: %2").arg(result.id, titleError));

    if (input.type != NoteNode::Type::Folder && input.type != NoteNode::Type::Note)
        return fail(QStringLiteral("Node %1 has an invalid type.").arg(result.id));
    if (input.createdAt < 0 || input.updatedAt < 0)
        return fail(QStringLiteral("Node %1 has a negative timestamp.").arg(result.id));

    // Note objects are always stored as <uuid>.bin, regardless of their title.
    const QString expectedFileName = result.id + kNoteSuffix;
    if (result.isFolder()) {
        if (!input.fileName.isEmpty())
            return fail(QStringLiteral("Folder %1 cannot have a note file name.").arg(result.id));
        result.fileName.clear();
    } else {
        if (!input.fileName.isEmpty() && input.fileName != expectedFileName)
            return fail(QStringLiteral("Note %1 must use the encrypted file name %2.")
                            .arg(result.id, expectedFileName));
        result.fileName = expectedFileName;
    }

    *output = result;
    return true;
}

bool validateNodeGraph(const QHash<QString, NoteNode> &nodes, QString *error)
{
    for (auto it = nodes.cbegin(); it != nodes.cend(); ++it) {
        const NoteNode &node = it.value();
        if (!node.parentId.isEmpty()) {
            const auto parent = nodes.constFind(node.parentId);
            if (parent == nodes.cend()) {
                if (error)
                    *error = QStringLiteral("Node %1 has a missing parent.").arg(node.id);
                return false;
            }
            if (!parent->isFolder()) {
                if (error)
                    *error = QStringLiteral("Node %1 has a non-folder parent.").arg(node.id);
                return false;
            }
        }

        QSet<QString> visited;
        QString current = node.id;
        while (!current.isEmpty()) {
            if (visited.contains(current)) {
                if (error)
                    *error = QStringLiteral("The folder hierarchy contains a cycle at %1.").arg(current);
                return false;
            }
            visited.insert(current);
            current = nodes.value(current).parentId;
        }
    }
    return true;
}

bool nodeOrder(const NoteNode &left, const NoteNode &right)
{
    if (left.sortOrder != right.sortOrder)
        return left.sortOrder < right.sortOrder;
    if (left.title != right.title)
        return left.title < right.title;
    return left.id < right.id;
}
} // namespace

NotebookStore::NotebookStore() = default;

NotebookStore::~NotebookStore()
{
    close();
}

void NotebookStore::close()
{
    m_open = false;
    // Destroying the CryptoService wipes the derived key.
    m_crypto.reset();
    m_masterPassword.fill(QChar::Null);
    m_masterPassword.clear();
    m_nodes.clear();
    m_assets.clear();
    m_id.clear();
    m_name.clear();
    m_rootPath.clear();
    m_writerId.clear();
}

bool NotebookStore::setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

QString NotebookStore::internalPath(const QString &name) const
{
    return QDir(m_rootPath).filePath(name);
}

QString NotebookStore::noteFilePath(const QString &noteId) const
{
    const QString id = normalizedUuid(noteId);
    if (id.isEmpty())
        return QString();
    return internalPath(kNotesDirectory + QLatin1Char('/') + id + kNoteSuffix);
}

QString NotebookStore::notePath(const NoteNode &node) const
{
    return noteFilePath(node.id);
}

QString NotebookStore::assetPath(const QString &id) const
{
    const QString normalizedId = normalizedUuid(id);
    if (normalizedId.isEmpty())
        return QString();
    return internalPath(kAssetsDirectory + QLatin1Char('/') + normalizedId + kNoteSuffix);
}

bool NotebookStore::writeNoteFile(const QString &noteId, const QByteArray &plainText, QString *error) const
{
    const QString id = normalizedUuid(noteId);
    if (id.isEmpty())
        return setError(error, QStringLiteral("The note has an invalid UUID."));
    // Every call encrypts with a fresh nonce, so no two files ever share a (key, nonce) pair.
    QByteArray cipherText;
    if (!m_crypto->encrypt(plainText, noteAssociatedData(m_id, id), &cipherText, error))
        return false;
    return writeAtomically(noteFilePath(id), cipherText, error);
}

bool NotebookStore::writeAssetFile(const QString &assetId, const QByteArray &plainText, QString *error) const
{
    const QString id = normalizedUuid(assetId);
    if (id.isEmpty())
        return setError(error, QStringLiteral("The asset has an invalid UUID."));
    QByteArray cipherText;
    if (!m_crypto->encrypt(plainText, assetAssociatedData(m_id, id), &cipherText, error))
        return false;
    return writeAtomically(assetPath(id), cipherText, error);
}

bool NotebookStore::readEncryptedJson(const QString &path, const QByteArray &associatedData,
                                      QByteArray *cipherText, QJsonObject *object, QString *error) const
{
    cipherText->clear();
    if (!readFileBytes(path, cipherText, error))
        return false;
    QByteArray plainText;
    if (!m_crypto->decrypt(*cipherText, associatedData, &plainText, error))
        return false;
    return parseJsonObject(plainText, path, object, error);
}

bool NotebookStore::create(const QString &rootPath, const QString &name, const QString &writerId,
                           const QString &masterPassword, QString *error)
{
    return createWithId(rootPath, name, QUuid::createUuid().toString(QUuid::WithoutBraces), writerId,
                        masterPassword, error);
}

bool NotebookStore::createWithId(const QString &rootPath, const QString &name, const QString &notebookId,
                                 const QString &writerId, const QString &masterPassword, QString *error)
{
    if (error)
        error->clear();
    close();

    QString titleError;
    if (!validTitle(name, &titleError))
        return setError(error, QStringLiteral("Invalid notebook name: %1").arg(titleError));
    if (rootPath.trimmed().isEmpty())
        return setError(error, QStringLiteral("The notebook path must not be empty."));
    const QString cleanRoot = QDir::cleanPath(QDir(rootPath).absolutePath());
    if (QFileInfo::exists(cleanRoot) && !QFileInfo(cleanRoot).isDir())
        return setError(error, QStringLiteral("The notebook path is not a directory."));
    const QString id = normalizedUuid(notebookId);
    if (id.isEmpty())
        return setError(error, QStringLiteral("The notebook ID is invalid."));
    if (masterPassword.isEmpty())
        return setError(error, QStringLiteral("The master password must not be empty."));
    if (!CryptoService::isAvailable(error))
        return false;

    const QDir root(cleanRoot);
    if (QFileInfo::exists(root.filePath(kHeaderName))
        || QFileInfo::exists(root.filePath(kHeaderName + kBackupSuffix))
        || QFileInfo::exists(root.filePath(kIndexName))
        || QFileInfo::exists(root.filePath(kIndexName + kBackupSuffix))
        || QFileInfo::exists(root.filePath(kLegacyIndexName))) {
        return setError(error, QStringLiteral("A notebook already exists at this path."));
    }

    if (!QDir().mkpath(cleanRoot) || !QDir().mkpath(root.filePath(kNotesDirectory))
        || !QDir().mkpath(root.filePath(kAssetsDirectory))) {
        return setError(error, QStringLiteral("Cannot create the notebook directories."));
    }

    m_rootPath = cleanRoot;
    m_id = id;
    m_name = name;
    m_writerId = writerId.isEmpty()
                     ? QUuid::createUuid().toString(QUuid::WithoutBraces).toLower()
                     : writerId;
    // Own a private copy, so close() can wipe this buffer rather than a shared one.
    m_masterPassword = QString(masterPassword.constData(), masterPassword.size());
    m_crypto = std::make_unique<CryptoService>();

    const auto abandon = [this, &cleanRoot] {
        QFile::remove(internalPath(kHeaderName));
        QFile::remove(internalPath(kHeaderName) + kBackupSuffix);
        QFile::remove(internalPath(kIndexName));
        QFile::remove(internalPath(kIndexName) + kBackupSuffix);
        QDir(cleanRoot).rmdir(kNotesDirectory);
        QDir(cleanRoot).rmdir(kAssetsDirectory);
        QDir(cleanRoot).rmdir(kInternalDirectory);
        close();
    };

    // The crypto header exists before anything encrypted: no object can be written
    // until the key has been derived and its check has been stored.
    QJsonObject header;
    if (!m_crypto->createHeader(masterPassword, &header, error, true)) {
        abandon();
        return false;
    }
    header.insert(QStringLiteral("id"), m_id);
    const QByteArray headerData = QJsonDocument(header).toJson(QJsonDocument::Indented);
    if (!writeWithBackup(internalPath(kHeaderName), headerData, error)) {
        abandon();
        return false;
    }
    if (!saveIndex(error)) {
        abandon();
        return false;
    }

    m_open = true;
    return true;
}

bool NotebookStore::open(const QString &rootPath, const QString &masterPassword, const QString &writerId,
                         QString *error)
{
    if (error)
        error->clear();
    close();

    if (rootPath.trimmed().isEmpty())
        return setError(error, QStringLiteral("The notebook path must not be empty."));
    const QString cleanRoot = QDir::cleanPath(QDir(rootPath).absolutePath());
    if (!QFileInfo(cleanRoot).isDir())
        return setError(error, QStringLiteral("The notebook directory does not exist."));
    if (masterPassword.isEmpty())
        return setError(error, QStringLiteral("The master password must not be empty."));
    if (!CryptoService::isAvailable(error))
        return false;

    m_rootPath = cleanRoot;
    m_writerId = writerId.isEmpty()
                     ? QUuid::createUuid().toString(QUuid::WithoutBraces).toLower()
                     : writerId;
    m_masterPassword = QString(masterPassword.constData(), masterPassword.size());
    m_crypto = std::make_unique<CryptoService>();

    // Any failure below discards the key, the password, and every partially loaded value.
    if (!load(error)) {
        close();
        return false;
    }
    m_open = true;
    return true;
}

bool NotebookStore::load(QString *error)
{
    // 1. The non-secret header. Its backup is an identical plaintext copy of the same header.
    const QString headerPath = internalPath(kHeaderName);
    const QString headerBackupPath = headerPath + kBackupSuffix;
    QByteArray headerData;
    QJsonObject header;
    bool recoveredHeader = false;
    QString headerError;
    if (!readFileBytes(headerPath, &headerData, &headerError)
        || !parseJsonObject(headerData, headerPath, &header, &headerError)) {
        QString backupError;
        if (!readFileBytes(headerBackupPath, &headerData, &backupError)
            || !parseJsonObject(headerData, headerBackupPath, &header, &backupError)) {
            if (QFileInfo::exists(internalPath(kLegacyIndexName)))
                return setError(error, legacyNotebookMessage());
            return setError(error, headerError);
        }
        recoveredHeader = true;
    }

    // A metadata file without a crypto format belongs to an old plaintext notebook.
    if (!header.value(QStringLiteral("format")).isString())
        return setError(error, legacyNotebookMessage());

    // 2. Validate the header, then derive and verify the key before the index is touched.
    //    A wrong password or a header of another format leaves the key cleared.
    const QString headerId = normalizedUuid(header.value(QStringLiteral("id")).toString());
    if (headerId.isEmpty())
        return setError(error, QStringLiteral("The notebook header has an invalid UUID."));
    if (!m_crypto->initializeFromHeader(header, m_masterPassword, error, true))
        return false;

    // 3. The encrypted index. The backup is only used when the primary copy cannot be
    //    read, authenticated, or parsed; the verified ciphertext is then restored.
    const QString indexPath = internalPath(kIndexName);
    const QByteArray associatedData = indexAssociatedData(headerId);
    QByteArray indexData;
    QJsonObject index;
    bool recoveredIndex = false;
    QString indexError;
    if (!readEncryptedJson(indexPath, associatedData, &indexData, &index, &indexError)) {
        QString backupError;
        if (!readEncryptedJson(indexPath + kBackupSuffix, associatedData, &indexData, &index,
                              &backupError)) {
            return setError(error, QStringLiteral("Cannot read the encrypted node index: %1")
                                       .arg(indexError));
        }
        recoveredIndex = true;
    }

    if (index.value(QStringLiteral("version")).toInt(-1) != kSchemaVersion
        || !index.value(QStringLiteral("nodes")).isArray())
        return setError(error, QStringLiteral("Unsupported or invalid node index."));
    if (!index.value(QStringLiteral("notebookName")).isString())
        return setError(error, QStringLiteral("The node index has no valid notebook name."));
    QString titleError;
    if (!validTitle(index.value(QStringLiteral("notebookName")).toString(), &titleError))
        return setError(error, QStringLiteral("Invalid notebook name: %1").arg(titleError));

    QHash<QString, NoteNode> loaded;
    const QJsonArray array = index.value(QStringLiteral("nodes")).toArray();
    for (const QJsonValue &value : array) {
        if (!value.isObject())
            return setError(error, QStringLiteral("The node index contains a non-object entry."));
        const QJsonObject object = value.toObject();
        NoteNode candidate;
        candidate.id = object.value(QStringLiteral("id")).toString();
        candidate.parentId = object.value(QStringLiteral("parentId")).toString();
        const QString type = object.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("folder"))
            candidate.type = NoteNode::Type::Folder;
        else if (type == QStringLiteral("note"))
            candidate.type = NoteNode::Type::Note;
        else
            return setError(error, QStringLiteral("The node index contains an invalid type."));
        if (!object.value(QStringLiteral("title")).isString())
            return setError(error, QStringLiteral("A node has no valid title."));
        candidate.title = object.value(QStringLiteral("title")).toString();
        candidate.fileName = object.value(QStringLiteral("fileName")).toString();
        candidate.writerId = object.value(QStringLiteral("writerId")).toString();

        if (!jsonInt64(object, QStringLiteral("createdAt"), 0, &candidate.createdAt, error)
            || !jsonInt64(object, QStringLiteral("updatedAt"), 0, &candidate.updatedAt, error))
            return false;
        qint64 sortOrder = 0;
        if (!jsonInt64(object, QStringLiteral("sortOrder"), 0, &sortOrder, error))
            return false;
        if (sortOrder > std::numeric_limits<int>::max())
            return setError(error, QStringLiteral("A node has an invalid sort order."));
        candidate.sortOrder = static_cast<int>(sortOrder);
        const QJsonValue deleted = object.value(QStringLiteral("deleted"));
        if (!deleted.isUndefined() && !deleted.isBool())
            return setError(error, QStringLiteral("A node has an invalid tombstone flag."));
        candidate.deleted = deleted.toBool(false);

        NoteNode normalized;
        if (!normalizeNode(candidate, &normalized, error))
            return false;
        if (loaded.contains(normalized.id))
            return setError(error, QStringLiteral("The node index contains duplicate IDs."));
        loaded.insert(normalized.id, normalized);
    }
    if (!validateNodeGraph(loaded, error))
        return false;

    QHash<QString, AssetRecord> loadedAssets;
    const QJsonValue assetsValue = index.value(QStringLiteral("assets"));
    if (!assetsValue.isUndefined() && !assetsValue.isArray())
        return setError(error, QStringLiteral("The asset index is invalid."));
    const QJsonArray assetArray = assetsValue.toArray();
    for (const QJsonValue &value : assetArray) {
        if (!value.isObject())
            return setError(error, QStringLiteral("The asset index contains a non-object entry."));
        const QJsonObject object = value.toObject();
        if (!object.value(QStringLiteral("id")).isString()
            || !object.value(QStringLiteral("mimeType")).isString()
            || !object.value(QStringLiteral("extension")).isString()
            || !object.value(QStringLiteral("originalName")).isString()
            || !object.value(QStringLiteral("sha256")).isString()
            || !object.value(QStringLiteral("writerId")).isString()) {
            return setError(error, QStringLiteral("The asset index contains invalid metadata."));
        }
        AssetRecord candidate;
        candidate.id = object.value(QStringLiteral("id")).toString();
        candidate.mimeType = object.value(QStringLiteral("mimeType")).toString();
        candidate.extension = object.value(QStringLiteral("extension")).toString();
        candidate.originalName = object.value(QStringLiteral("originalName")).toString();
        candidate.sha256 = object.value(QStringLiteral("sha256")).toString();
        candidate.writerId = object.value(QStringLiteral("writerId")).toString();
        if (!jsonInt64(object, QStringLiteral("size"), 0, &candidate.size, error)
            || !jsonInt64(object, QStringLiteral("createdAt"), 0, &candidate.createdAt, error)
            || !jsonInt64(object, QStringLiteral("updatedAt"), 0, &candidate.updatedAt, error)) {
            return false;
        }
        const QJsonValue deleted = object.value(QStringLiteral("deleted"));
        if (!deleted.isUndefined() && !deleted.isBool())
            return setError(error, QStringLiteral("An asset has an invalid tombstone flag."));
        candidate.deleted = deleted.toBool(false);

        AssetRecord normalized;
        if (!normalizeAsset(candidate, &normalized, error))
            return false;
        if (loadedAssets.contains(normalized.id))
            return setError(error, QStringLiteral("The asset index contains duplicate IDs."));
        loadedAssets.insert(normalized.id, normalized);
    }

    // Everything is verified: publish the state.
    m_id = headerId;
    m_name = index.value(QStringLiteral("notebookName")).toString();
    m_nodes = loaded;
    m_assets = loadedAssets;

    // Restore a recovered primary copy from the verified backup, byte for byte.
    // This is a repair of an already consistent state, so a failure is not fatal.
    if (recoveredHeader || recoveredIndex) {
        QString ignored;
        if (recoveredHeader)
            writeAtomically(headerPath, headerData, &ignored);
        if (recoveredIndex)
            writeAtomically(indexPath, indexData, &ignored);
    }
    return true;
}

bool NotebookStore::saveIndex(QString *error) const
{
    QJsonArray array;
    const QVector<NoteNode> ordered = nodes(true);
    for (const NoteNode &node : ordered)
        array.append(nodeToJson(node));
    QJsonArray assetArray;
    const QVector<AssetRecord> orderedAssets = assets(true);
    for (const AssetRecord &asset : orderedAssets)
        assetArray.append(assetToJson(asset));
    // The notebook name lives in the encrypted index; the header never contains it.
    const QJsonObject index{
        {QStringLiteral("version"), kSchemaVersion},
        {QStringLiteral("notebookName"), m_name},
        {QStringLiteral("nodes"), array},
        {QStringLiteral("assets"), assetArray}
    };
    // Every save encrypts with a fresh nonce. The backup is a byte-identical copy.
    QByteArray cipherText;
    if (!m_crypto->encrypt(QJsonDocument(index).toJson(QJsonDocument::Indented),
                           indexAssociatedData(m_id), &cipherText, error))
        return false;
    return writeWithBackup(internalPath(kIndexName), cipherText, error);
}

QVector<NoteNode> NotebookStore::nodes(bool includeDeleted) const
{
    QVector<NoteNode> result;
    result.reserve(m_nodes.size());
    for (auto it = m_nodes.cbegin(); it != m_nodes.cend(); ++it) {
        if (includeDeleted || !it->deleted)
            result.append(it.value());
    }
    std::sort(result.begin(), result.end(), nodeOrder);
    return result;
}

NoteNode NotebookStore::node(const QString &id) const
{
    return m_nodes.value(normalizedUuid(id));
}

QStringList NotebookStore::childIds(const QString &parentId, bool includeDeleted) const
{
    const QString normalizedParent = parentId.isEmpty() ? QString() : normalizedUuid(parentId);
    QVector<NoteNode> children;
    for (auto it = m_nodes.cbegin(); it != m_nodes.cend(); ++it) {
        if (it->parentId == normalizedParent && (includeDeleted || !it->deleted))
            children.append(it.value());
    }
    std::sort(children.begin(), children.end(), nodeOrder);
    QStringList result;
    result.reserve(children.size());
    for (const NoteNode &child : children)
        result.append(child.id);
    return result;
}

QStringList NotebookStore::subtreeIds(const QString &id) const
{
    QStringList result;
    const QString root = normalizedUuid(id);
    if (!m_nodes.contains(root))
        return result;

    QStringList pending{root};
    QSet<QString> visited;
    while (!pending.isEmpty()) {
        const QString current = pending.takeLast();
        if (visited.contains(current))
            continue;
        visited.insert(current);
        result.append(current);
        QStringList children = childIds(current, true);
        std::reverse(children.begin(), children.end());
        pending.append(children);
    }
    return result;
}

bool NotebookStore::setName(const QString &name, QString *error)
{
    if (error)
        error->clear();
    if (!m_open)
        return setError(error, QStringLiteral("No notebook is open."));
    QString titleError;
    if (!validTitle(name, &titleError))
        return setError(error, QStringLiteral("Invalid notebook name: %1").arg(titleError));
    const QString oldName = m_name;
    m_name = name;
    // Only the encrypted index changes; the non-secret header stays untouched.
    if (!saveIndex(error)) {
        m_name = oldName;
        return false;
    }
    return true;
}

QString NotebookStore::validateNewParent(const QString &id, const QString &newParentId) const
{
    if (newParentId.isEmpty())
        return QString();
    const QString parentId = normalizedUuid(newParentId);
    if (parentId.isEmpty())
        return QStringLiteral("The new parent has an invalid UUID.");
    if (parentId == normalizedUuid(id))
        return QStringLiteral("A node cannot be its own parent.");
    const auto parent = m_nodes.constFind(parentId);
    if (parent == m_nodes.cend() || !parent->isFolder() || parent->deleted)
        return QStringLiteral("The new parent must be an existing, active folder.");
    if (!id.isEmpty() && subtreeIds(id).contains(parentId))
        return QStringLiteral("Moving this folder there would create a cycle.");
    return QString();
}

QString NotebookStore::createNode(NoteNode::Type type, const QString &parentId,
                                  const QString &title, QString *error)
{
    if (error)
        error->clear();
    if (!m_open) {
        setError(error, QStringLiteral("No notebook is open."));
        return QString();
    }
    if (type != NoteNode::Type::Folder && type != NoteNode::Type::Note) {
        setError(error, QStringLiteral("Invalid node type."));
        return QString();
    }
    QString titleError;
    if (!validTitle(title, &titleError)) {
        setError(error, QStringLiteral("Invalid node title: %1").arg(titleError));
        return QString();
    }
    const QString parentError = validateNewParent(QString(), parentId);
    if (!parentError.isEmpty()) {
        setError(error, parentError);
        return QString();
    }

    NoteNode created;
    created.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
    created.parentId = parentId.isEmpty() ? QString() : normalizedUuid(parentId);
    created.type = type;
    created.title = title;
    created.fileName = created.isNote() ? created.id + kNoteSuffix : QString();
    created.createdAt = QDateTime::currentMSecsSinceEpoch();
    created.updatedAt = created.createdAt;
    created.writerId = m_writerId;

    int maximumSortOrder = -1;
    for (auto it = m_nodes.cbegin(); it != m_nodes.cend(); ++it) {
        if (it->parentId == created.parentId)
            maximumSortOrder = std::max(maximumSortOrder, it->sortOrder);
    }
    created.sortOrder = maximumSortOrder == std::numeric_limits<int>::max()
                            ? maximumSortOrder
                            : maximumSortOrder + 1;

    // A new note starts as an encrypted empty document.
    if (created.isNote() && !writeNoteFile(created.id, QByteArray(), error))
        return QString();

    m_nodes.insert(created.id, created);
    if (!saveIndex(error)) {
        m_nodes.remove(created.id);
        if (created.isNote())
            QFile::remove(notePath(created));
        return QString();
    }
    return created.id;
}

bool NotebookStore::renameNode(const QString &id, const QString &title, QString *error)
{
    if (error)
        error->clear();
    if (!m_open)
        return setError(error, QStringLiteral("No notebook is open."));
    const QString normalizedId = normalizedUuid(id);
    auto it = m_nodes.find(normalizedId);
    if (it == m_nodes.end() || it->deleted)
        return setError(error, QStringLiteral("The node does not exist or is deleted."));
    QString titleError;
    if (!validTitle(title, &titleError))
        return setError(error, QStringLiteral("Invalid node title: %1").arg(titleError));

    const NoteNode oldNode = it.value();
    it->title = title;
    it->updatedAt = QDateTime::currentMSecsSinceEpoch();
    it->writerId = m_writerId;
    if (!saveIndex(error)) {
        it.value() = oldNode;
        return false;
    }
    return true;
}

bool NotebookStore::moveNode(const QString &id, const QString &newParentId, int sortOrder, QString *error)
{
    if (error)
        error->clear();
    if (!m_open)
        return setError(error, QStringLiteral("No notebook is open."));
    const QString normalizedId = normalizedUuid(id);
    auto it = m_nodes.find(normalizedId);
    if (it == m_nodes.end() || it->deleted)
        return setError(error, QStringLiteral("The node does not exist or is deleted."));
    const QString parentError = validateNewParent(normalizedId, newParentId);
    if (!parentError.isEmpty())
        return setError(error, parentError);

    const NoteNode oldNode = it.value();
    it->parentId = newParentId.isEmpty() ? QString() : normalizedUuid(newParentId);
    if (sortOrder >= 0)
        it->sortOrder = sortOrder;
    it->updatedAt = QDateTime::currentMSecsSinceEpoch();
    it->writerId = m_writerId;
    if (!saveIndex(error)) {
        it.value() = oldNode;
        return false;
    }
    return true;
}

bool NotebookStore::removeNode(const QString &id, QString *error)
{
    if (error)
        error->clear();
    if (!m_open)
        return setError(error, QStringLiteral("No notebook is open."));
    const QString normalizedId = normalizedUuid(id);
    if (!m_nodes.contains(normalizedId) || m_nodes.value(normalizedId).deleted)
        return setError(error, QStringLiteral("The node does not exist or is already deleted."));

    const QHash<QString, NoteNode> oldNodes = m_nodes;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QStringList ids = subtreeIds(normalizedId);
    for (const QString &nodeId : ids) {
        auto it = m_nodes.find(nodeId);
        if (it != m_nodes.end() && !it->deleted) {
            it->deleted = true;
            it->updatedAt = now;
            it->writerId = m_writerId;
        }
    }
    if (!saveIndex(error)) {
        m_nodes = oldNodes;
        return false;
    }
    return true;
}

QString NotebookStore::readNote(const QString &id, QString *error) const
{
    if (error)
        error->clear();
    if (!m_open) {
        setError(error, QStringLiteral("No notebook is open."));
        return QString();
    }
    const NoteNode note = node(id);
    if (note.id.isEmpty() || !note.isNote() || note.deleted) {
        setError(error, QStringLiteral("The note does not exist or is deleted."));
        return QString();
    }

    QFile file(notePath(note));
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("Cannot read note: %1").arg(file.errorString()));
        return QString();
    }
    QByteArray plainText;
    if (!m_crypto->decrypt(file.readAll(), noteAssociatedData(m_id, note.id), &plainText, error))
        return QString();
    return QString::fromUtf8(plainText);
}

bool NotebookStore::writeNote(const QString &id, const QString &content, QString *error)
{
    if (error)
        error->clear();
    if (!m_open)
        return setError(error, QStringLiteral("No notebook is open."));
    auto it = m_nodes.find(normalizedUuid(id));
    if (it == m_nodes.end() || !it->isNote() || it->deleted)
        return setError(error, QStringLiteral("The note does not exist or is deleted."));

    // Keep the previous ciphertext, so a rollback restores the exact bytes from disk.
    const QString path = notePath(it.value());
    QByteArray oldCipherText;
    QFile oldFile(path);
    if (!oldFile.open(QIODevice::ReadOnly))
        return setError(error, QStringLiteral("Cannot read the previous note content: %1").arg(oldFile.errorString()));
    oldCipherText = oldFile.readAll();
    oldFile.close();

    if (!writeNoteFile(it->id, content.toUtf8(), error))
        return false;

    const NoteNode oldNode = it.value();
    it->updatedAt = QDateTime::currentMSecsSinceEpoch();
    it->writerId = m_writerId;
    if (!saveIndex(error)) {
        it.value() = oldNode;
        QString rollbackError;
        if (!writeAtomically(path, oldCipherText, &rollbackError) && error)
            error->append(QStringLiteral(" Content rollback also failed: %1").arg(rollbackError));
        return false;
    }
    return true;
}

QVector<AssetRecord> NotebookStore::assets(bool includeDeleted) const
{
    QVector<AssetRecord> result;
    result.reserve(m_assets.size());
    for (auto it = m_assets.cbegin(); it != m_assets.cend(); ++it) {
        if (includeDeleted || !it->deleted)
            result.append(it.value());
    }
    std::sort(result.begin(), result.end(), [](const AssetRecord &left, const AssetRecord &right) {
        return left.id < right.id;
    });
    return result;
}

AssetRecord NotebookStore::asset(const QString &id) const
{
    return m_assets.value(normalizedUuid(id));
}

QByteArray NotebookStore::readAsset(const QString &id, QString *error) const
{
    if (error)
        error->clear();
    if (!m_open) {
        setError(error, QStringLiteral("No notebook is open."));
        return QByteArray();
    }
    const AssetRecord saved = asset(id);
    if (saved.id.isEmpty() || saved.deleted) {
        setError(error, QStringLiteral("The asset does not exist or is deleted."));
        return QByteArray();
    }

    QByteArray cipherText;
    if (!readFileBytes(assetPath(saved.id), &cipherText, error))
        return QByteArray();
    QByteArray plainText;
    if (!m_crypto->decrypt(cipherText, assetAssociatedData(m_id, saved.id), &plainText, error))
        return QByteArray();
    return plainText;
}

bool NotebookStore::writeAsset(const AssetRecord &input, const QByteArray &data, QString *error)
{
    if (error)
        error->clear();
    if (!m_open)
        return setError(error, QStringLiteral("No notebook is open."));

    AssetRecord result;
    if (!normalizeAsset(input, &result, error))
        return false;
    const auto old = m_assets.constFind(result.id);
    const bool hadOldAsset = old != m_assets.cend();
    const AssetRecord oldAsset = hadOldAsset ? old.value() : AssetRecord();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (result.createdAt == 0)
        result.createdAt = hadOldAsset && oldAsset.createdAt != 0 ? oldAsset.createdAt : now;
    result.updatedAt = now;
    result.writerId = m_writerId;
    result.size = data.size();
    result.sha256 = QString::fromLatin1(
        QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());

    const bool writeContent = !result.deleted;
    const QString path = assetPath(result.id);
    QByteArray oldCipherText;
    bool hadOldContent = false;
    if (writeContent && !QDir().mkpath(internalPath(kAssetsDirectory)))
        return setError(error, QStringLiteral("Cannot create the asset directory."));
    if (writeContent && QFileInfo::exists(path)) {
        if (!readFileBytes(path, &oldCipherText, error))
            return false;
        hadOldContent = true;
    }
    if (writeContent && !writeAssetFile(result.id, data, error))
        return false;

    m_assets.insert(result.id, result);
    if (!saveIndex(error)) {
        if (hadOldAsset)
            m_assets.insert(result.id, oldAsset);
        else
            m_assets.remove(result.id);
        if (writeContent) {
            QString rollbackError;
            const bool rolledBack = hadOldContent
                ? writeAtomically(path, oldCipherText, &rollbackError)
                : QFile::remove(path);
            if (!rolledBack && error) {
                error->append(QStringLiteral(" Asset content rollback also failed: %1")
                                  .arg(rollbackError));
            }
        }
        return false;
    }
    return true;
}

bool NotebookStore::applyRemoteAsset(const AssetRecord &input, const QByteArray &data, QString *error)
{
    if (error)
        error->clear();
    if (!m_open)
        return setError(error, QStringLiteral("No notebook is open."));

    AssetRecord remote;
    if (!normalizeAsset(input, &remote, error))
        return false;
    if (!remote.deleted) {
        const QString digest = QString::fromLatin1(
            QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
        if (remote.size != data.size() || remote.sha256.compare(digest, Qt::CaseInsensitive) != 0)
            return setError(error, QStringLiteral("The remote asset content does not match its metadata."));
    }

    const bool writeContent = !remote.deleted;
    const QString path = assetPath(remote.id);
    QByteArray oldCipherText;
    bool hadOldContent = false;
    if (writeContent && !QDir().mkpath(internalPath(kAssetsDirectory)))
        return setError(error, QStringLiteral("Cannot create the asset directory."));
    if (writeContent && QFileInfo::exists(path)) {
        if (!readFileBytes(path, &oldCipherText, error))
            return false;
        hadOldContent = true;
    }
    if (writeContent && !writeAssetFile(remote.id, data, error))
        return false;

    const auto old = m_assets.constFind(remote.id);
    const bool hadOldAsset = old != m_assets.cend();
    const AssetRecord oldAsset = hadOldAsset ? old.value() : AssetRecord();
    m_assets.insert(remote.id, remote);
    if (!saveIndex(error)) {
        if (hadOldAsset)
            m_assets.insert(remote.id, oldAsset);
        else
            m_assets.remove(remote.id);
        if (writeContent) {
            QString rollbackError;
            const bool rolledBack = hadOldContent
                ? writeAtomically(path, oldCipherText, &rollbackError)
                : QFile::remove(path);
            if (!rolledBack && error) {
                error->append(QStringLiteral(" Asset content rollback also failed: %1")
                                  .arg(rollbackError));
            }
        }
        return false;
    }
    return true;
}

bool NotebookStore::applyRemoteNode(const NoteNode &node, const QString &content, QString *error)
{
    if (error)
        error->clear();
    if (!m_open)
        return setError(error, QStringLiteral("No notebook is open."));

    NoteNode remote;
    if (!normalizeNode(node, &remote, error))
        return false;
    if (!remote.parentId.isEmpty()) {
        const auto parent = m_nodes.constFind(remote.parentId);
        if (parent == m_nodes.cend() || !parent->isFolder())
            return setError(error, QStringLiteral("The remote node has an invalid parent."));
        if (parent->deleted && !remote.deleted)
            return setError(error, QStringLiteral("An active remote node cannot have a deleted parent."));
    }

    QHash<QString, NoteNode> prospective = m_nodes;
    prospective.insert(remote.id, remote);
    if (!validateNodeGraph(prospective, error))
        return false;

    const bool writeContent = remote.isNote() && !remote.deleted;
    const QString path = notePath(remote);
    // The previous ciphertext is kept verbatim for a rollback; plaintext never touches disk.
    QByteArray oldCipherText;
    bool hadOldContent = false;
    if (writeContent && QFileInfo::exists(path)) {
        QFile oldFile(path);
        if (!oldFile.open(QIODevice::ReadOnly))
            return setError(error, QStringLiteral("Cannot read the previous note content: %1").arg(oldFile.errorString()));
        oldCipherText = oldFile.readAll();
        oldFile.close();
        hadOldContent = true;
    }
    if (writeContent && !writeNoteFile(remote.id, content.toUtf8(), error))
        return false;

    const QHash<QString, NoteNode> oldNodes = m_nodes;
    m_nodes = prospective;
    if (!saveIndex(error)) {
        m_nodes = oldNodes;
        if (writeContent) {
            QString rollbackError;
            const bool rolledBack = hadOldContent ? writeAtomically(path, oldCipherText, &rollbackError)
                                                  : QFile::remove(path);
            if (!rolledBack && error)
                error->append(QStringLiteral(" Content rollback also failed: %1").arg(rollbackError));
        }
        return false;
    }
    return true;
}
