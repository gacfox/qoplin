#include "storage/NotebookStore.h"

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

bool readJsonObject(const QString &path, QJsonObject *object, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("Cannot read %1: %2").arg(path, file.errorString());
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
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

    const QString expectedFileName = result.id + QStringLiteral(".md");
    if (result.isFolder()) {
        if (!input.fileName.isEmpty())
            return fail(QStringLiteral("Folder %1 cannot have a note file name.").arg(result.id));
        result.fileName.clear();
    } else {
        if (!input.fileName.isEmpty() && input.fileName != expectedFileName)
            return fail(QStringLiteral("Note %1 must use the flat file name %2.")
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

bool NotebookStore::create(const QString &rootPath, const QString &name,
                           const QString &writerId, QString *error)
{
    return createWithId(rootPath, name, QUuid::createUuid().toString(QUuid::WithoutBraces), writerId, error);
}

bool NotebookStore::createWithId(const QString &rootPath, const QString &name, const QString &notebookId,
                                 const QString &writerId, QString *error)
{
    if (error)
        error->clear();
    m_open = false;
    m_nodes.clear();

    QString titleError;
    if (!validTitle(name, &titleError))
        return setError(error, QStringLiteral("Invalid notebook name: %1").arg(titleError));
    if (rootPath.trimmed().isEmpty())
        return setError(error, QStringLiteral("The notebook path must not be empty."));

    const QString cleanRoot = QDir::cleanPath(QDir(rootPath).absolutePath());
    if (QFileInfo::exists(cleanRoot) && !QFileInfo(cleanRoot).isDir())
        return setError(error, QStringLiteral("The notebook path is not a directory."));
    if (!QDir().mkpath(cleanRoot)
        || !QDir().mkpath(QDir(cleanRoot).filePath(QStringLiteral(".qoplin/notes"))))
        return setError(error, QStringLiteral("Cannot create the notebook directories."));

    if (QFileInfo::exists(QDir(cleanRoot).filePath(QStringLiteral("qoplin.json")))
        || QFileInfo::exists(QDir(cleanRoot).filePath(QStringLiteral(".qoplin/index.json"))))
        return setError(error, QStringLiteral("A notebook already exists at this path."));

    m_rootPath = cleanRoot;
    m_id = normalizedUuid(notebookId);
    if (m_id.isEmpty())
        return setError(error, QStringLiteral("The notebook ID is invalid."));
    m_name = name;
    m_writerId = writerId.isEmpty()
                     ? QUuid::createUuid().toString(QUuid::WithoutBraces).toLower()
                     : writerId;

    if (!saveIndex(error) || !saveMetadata(error)) {
        QFile::remove(internalPath(QStringLiteral("qoplin.json")));
        QFile::remove(internalPath(QStringLiteral("qoplin.json.bak")));
        QFile::remove(internalPath(QStringLiteral(".qoplin/index.json")));
        QFile::remove(internalPath(QStringLiteral(".qoplin/index.json.bak")));
        QDir(cleanRoot).rmdir(QStringLiteral(".qoplin/notes"));
        QDir(cleanRoot).rmdir(QStringLiteral(".qoplin"));
        m_open = false;
        m_nodes.clear();
        return false;
    }
    m_open = true;
    return true;
}

bool NotebookStore::open(const QString &rootPath, QString *error)
{
    if (error)
        error->clear();
    m_open = false;
    m_nodes.clear();
    m_id.clear();
    m_name.clear();
    m_writerId.clear();

    if (rootPath.trimmed().isEmpty())
        return setError(error, QStringLiteral("The notebook path must not be empty."));
    const QString cleanRoot = QDir::cleanPath(QDir(rootPath).absolutePath());
    if (!QFileInfo(cleanRoot).isDir())
        return setError(error, QStringLiteral("The notebook directory does not exist."));

    m_rootPath = cleanRoot;
    if (!load(error)) {
        m_open = false;
        m_nodes.clear();
        return false;
    }
    m_open = true;
    return true;
}

bool NotebookStore::load(QString *error)
{
    const QString metadataPath = internalPath(QStringLiteral("qoplin.json"));
    const QString backupPath = metadataPath + QStringLiteral(".bak");
    QJsonObject metadata;
    QString metadataError;
    bool recovered = false;
    if (!readJsonObject(metadataPath, &metadata, &metadataError)) {
        if (!readJsonObject(backupPath, &metadata, error))
            return setError(error, metadataError);
        recovered = true;
    }

    if (metadata.value(QStringLiteral("version")).toInt(-1) != kSchemaVersion)
        return setError(error, QStringLiteral("Unsupported notebook metadata version."));
    const QString metadataId = normalizedUuid(metadata.value(QStringLiteral("id")).toString());
    if (metadataId.isEmpty())
        return setError(error, QStringLiteral("The notebook metadata has an invalid UUID."));
    if (!metadata.value(QStringLiteral("name")).isString())
        return setError(error, QStringLiteral("The notebook metadata has no valid name."));
    QString titleError;
    if (!validTitle(metadata.value(QStringLiteral("name")).toString(), &titleError))
        return setError(error, QStringLiteral("Invalid notebook name: %1").arg(titleError));

    m_id = metadataId;
    m_name = metadata.value(QStringLiteral("name")).toString();
    m_writerId = metadata.value(QStringLiteral("writerId")).toString();
    if (m_writerId.isEmpty())
        m_writerId = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();

    const QString indexPath = internalPath(QStringLiteral(".qoplin/index.json"));
    QJsonObject index;
    bool recoveredIndex = false;
    if (!readJsonObject(indexPath, &index, error)) {
        if (!readJsonObject(indexPath + QStringLiteral(".bak"), &index, error))
            return false;
        recoveredIndex = true;
    }
    if (index.value(QStringLiteral("version")).toInt(-1) != kSchemaVersion
        || !index.value(QStringLiteral("nodes")).isArray())
        return setError(error, QStringLiteral("Unsupported or invalid node index."));

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
    m_nodes = loaded;

    if (recovered) {
        QString ignored;
        writeAtomically(metadataPath,
                        QJsonDocument(metadata).toJson(QJsonDocument::Indented), &ignored);
    }
    if (!metadata.contains(QStringLiteral("writerId"))) {
        if (!saveMetadata(error))
            return false;
    }
    if (recoveredIndex && !saveIndex(error))
        return false;
    return true;
}

bool NotebookStore::saveMetadata(QString *error) const
{
    const QJsonObject metadata{
        {QStringLiteral("version"), kSchemaVersion},
        {QStringLiteral("id"), m_id},
        {QStringLiteral("name"), m_name},
        {QStringLiteral("writerId"), m_writerId}
    };
    const QByteArray data = QJsonDocument(metadata).toJson(QJsonDocument::Indented);
    const QString path = internalPath(QStringLiteral("qoplin.json"));
    if (!writeAtomically(path, data, error))
        return false;
    QString backupError;
    writeAtomically(path + QStringLiteral(".bak"), data, &backupError);
    return true;
}

bool NotebookStore::saveIndex(QString *error) const
{
    QJsonArray array;
    const QVector<NoteNode> ordered = nodes(true);
    for (const NoteNode &node : ordered)
        array.append(nodeToJson(node));
    const QJsonObject index{
        {QStringLiteral("version"), kSchemaVersion},
        {QStringLiteral("nodes"), array}
    };
    const QByteArray data = QJsonDocument(index).toJson(QJsonDocument::Indented);
    const QString path = internalPath(QStringLiteral(".qoplin/index.json"));
    if (!writeAtomically(path, data, error))
        return false;
    QString backupError;
    writeAtomically(path + QStringLiteral(".bak"), data, &backupError);
    return true;
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
    if (!saveMetadata(error)) {
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
    created.fileName = created.isNote() ? created.id + QStringLiteral(".md") : QString();
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

    if (created.isNote()
        && !writeAtomically(notePath(created), QByteArray(), error))
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

QString NotebookStore::notePath(const NoteNode &node) const
{
    const QString id = normalizedUuid(node.id);
    if (id.isEmpty())
        return QString();
    return internalPath(QStringLiteral(".qoplin/notes/") + id + QStringLiteral(".md"));
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
    return QString::fromUtf8(file.readAll());
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
    const QString path = notePath(it.value());
    QByteArray oldContent;
    QFile oldFile(path);
    if (!oldFile.open(QIODevice::ReadOnly))
        return setError(error, QStringLiteral("Cannot read the previous note content: %1").arg(oldFile.errorString()));
    oldContent = oldFile.readAll();
    oldFile.close();
    if (!writeAtomically(path, content.toUtf8(), error))
        return false;

    const NoteNode oldNode = it.value();
    it->updatedAt = QDateTime::currentMSecsSinceEpoch();
    it->writerId = m_writerId;
    if (!saveIndex(error)) {
        it.value() = oldNode;
        QString rollbackError;
        if (!writeAtomically(path, oldContent, &rollbackError) && error)
            error->append(QStringLiteral(" Content rollback also failed: %1").arg(rollbackError));
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
    QByteArray oldContent;
    bool hadOldContent = false;
    if (writeContent && QFileInfo::exists(path)) {
        QFile oldFile(path);
        if (!oldFile.open(QIODevice::ReadOnly))
            return setError(error, QStringLiteral("Cannot read the previous note content: %1").arg(oldFile.errorString()));
        oldContent = oldFile.readAll();
        oldFile.close();
        hadOldContent = true;
    }
    if (writeContent && !writeAtomically(path, content.toUtf8(), error))
        return false;

    const QHash<QString, NoteNode> oldNodes = m_nodes;
    m_nodes = prospective;
    if (!saveIndex(error)) {
        m_nodes = oldNodes;
        if (writeContent) {
            QString rollbackError;
            const bool rolledBack = hadOldContent ? writeAtomically(path, oldContent, &rollbackError)
                                                  : QFile::remove(path);
            if (!rolledBack && error)
                error->append(QStringLiteral(" Content rollback also failed: %1").arg(rollbackError));
        }
        return false;
    }
    return true;
}
