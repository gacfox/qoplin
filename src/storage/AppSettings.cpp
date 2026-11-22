#include "storage/AppSettings.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QUuid>

namespace
{
const char kNotebookArray[] = "notebooks";

QString comparablePath(const QString &path)
{
    if (path.isEmpty())
        return QString();
    QString result = QDir::cleanPath(QDir(path).absolutePath());
#ifdef Q_OS_WIN
    result = result.toCaseFolded();
#endif
    return result;
}

QList<NotebookInfo> readNotebooks(QSettings &settings)
{
    QList<NotebookInfo> result;
    const int size = settings.beginReadArray(kNotebookArray);
    for (int i = 0; i < size; ++i) {
        settings.setArrayIndex(i);
        NotebookInfo info;
        info.id = settings.value(QStringLiteral("id")).toString();
        info.name = settings.value(QStringLiteral("name")).toString();
        info.rootPath = settings.value(QStringLiteral("rootPath")).toString();
        info.lastOpened = settings.value(QStringLiteral("lastOpened"), 0).toLongLong();
        if (!info.rootPath.isEmpty())
            result.append(info);
    }
    settings.endArray();
    return result;
}

void writeNotebooks(QSettings &settings, const QList<NotebookInfo> &notebooks)
{
    settings.remove(QLatin1String(kNotebookArray));
    settings.beginWriteArray(kNotebookArray);
    for (int i = 0; i < notebooks.size(); ++i) {
        settings.setArrayIndex(i);
        const NotebookInfo &info = notebooks.at(i);
        settings.setValue(QStringLiteral("id"), info.id);
        settings.setValue(QStringLiteral("name"), info.name);
        settings.setValue(QStringLiteral("rootPath"), info.rootPath);
        settings.setValue(QStringLiteral("lastOpened"), info.lastOpened);
    }
    settings.endArray();
    settings.sync();
}
} // namespace

AppSettings::AppSettings()
    : m_settings(QStringLiteral("Qoplin"), QStringLiteral("Qoplin"))
{
}

QList<NotebookInfo> AppSettings::notebooks() const
{
    return readNotebooks(m_settings);
}

void AppSettings::registerNotebook(const NotebookInfo &info)
{
    if (info.rootPath.isEmpty())
        return;
    QList<NotebookInfo> registered = readNotebooks(m_settings);
    bool replaced = false;
    for (NotebookInfo &existing : registered) {
        if (comparablePath(existing.rootPath) == comparablePath(info.rootPath)) {
            existing = info;
            replaced = true;
            break;
        }
    }
    if (!replaced)
        registered.append(info);
    writeNotebooks(m_settings, registered);
}

void AppSettings::unregisterNotebook(const QString &rootPath)
{
    QList<NotebookInfo> registered = readNotebooks(m_settings);
    const QString path = comparablePath(rootPath);
    for (auto it = registered.begin(); it != registered.end();) {
        if (comparablePath(it->rootPath) == path)
            it = registered.erase(it);
        else
            ++it;
    }
    writeNotebooks(m_settings, registered);
    if (comparablePath(lastNotebook()) == path && !path.isEmpty()) {
        m_settings.remove(QStringLiteral("lastNotebook"));
        m_settings.sync();
    }
}

void AppSettings::renameNotebook(const QString &rootPath, const QString &name)
{
    QList<NotebookInfo> registered = readNotebooks(m_settings);
    const QString path = comparablePath(rootPath);
    for (NotebookInfo &info : registered) {
        if (comparablePath(info.rootPath) == path)
            info.name = name;
    }
    writeNotebooks(m_settings, registered);
}

void AppSettings::touchNotebook(const QString &rootPath)
{
    QList<NotebookInfo> registered = readNotebooks(m_settings);
    const QString path = comparablePath(rootPath);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (NotebookInfo &info : registered) {
        if (comparablePath(info.rootPath) == path)
            info.lastOpened = now;
    }
    writeNotebooks(m_settings, registered);
}

void AppSettings::removeMissingNotebooks()
{
    QList<NotebookInfo> registered = readNotebooks(m_settings);
    QList<NotebookInfo> present;
    for (const NotebookInfo &info : registered) {
        if (QFileInfo(info.rootPath).isDir())
            present.append(info);
    }
    writeNotebooks(m_settings, present);

    const QString last = lastNotebook();
    if (!last.isEmpty() && !QFileInfo(last).isDir()) {
        m_settings.remove(QStringLiteral("lastNotebook"));
        m_settings.sync();
    }
}

bool AppSettings::autoOpenLast() const
{
    return m_settings.value(QStringLiteral("autoOpenLast"), true).toBool();
}

void AppSettings::setAutoOpenLast(bool value)
{
    m_settings.setValue(QStringLiteral("autoOpenLast"), value);
    m_settings.sync();
}

QString AppSettings::lastNotebook() const
{
    return m_settings.value(QStringLiteral("lastNotebook")).toString();
}

void AppSettings::setLastNotebook(const QString &rootPath)
{
    if (rootPath.isEmpty())
        m_settings.remove(QStringLiteral("lastNotebook"));
    else
        m_settings.setValue(QStringLiteral("lastNotebook"), rootPath);
    m_settings.sync();
}

QString AppSettings::deviceId() const
{
    QString id = m_settings.value(QStringLiteral("deviceId")).toString();
    if (id.isEmpty()) {
        id = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
        m_settings.setValue(QStringLiteral("deviceId"), id);
        m_settings.sync();
    }
    return id;
}

QByteArray AppSettings::windowGeometry() const
{
    return m_settings.value(QStringLiteral("windowGeometry")).toByteArray();
}

void AppSettings::setWindowGeometry(const QByteArray &geometry)
{
    m_settings.setValue(QStringLiteral("windowGeometry"), geometry);
    m_settings.sync();
}

QByteArray AppSettings::splitterState() const
{
    return m_settings.value(QStringLiteral("splitterState")).toByteArray();
}

void AppSettings::setSplitterState(const QByteArray &state)
{
    m_settings.setValue(QStringLiteral("splitterState"), state);
    m_settings.sync();
}

int AppSettings::editorMode() const
{
    return m_settings.value(QStringLiteral("editorMode"), 0).toInt();
}

void AppSettings::setEditorMode(int mode)
{
    m_settings.setValue(QStringLiteral("editorMode"), mode);
    m_settings.sync();
}

QString AppSettings::editorFontFamily() const
{
    return m_settings.value(QStringLiteral("editorFontFamily"), QStringLiteral("Consolas")).toString();
}

void AppSettings::setEditorFontFamily(const QString &family)
{
    m_settings.setValue(QStringLiteral("editorFontFamily"), family);
    m_settings.sync();
}

int AppSettings::editorFontSize() const
{
    return m_settings.value(QStringLiteral("editorFontSize"), 11).toInt();
}

void AppSettings::setEditorFontSize(int size)
{
    m_settings.setValue(QStringLiteral("editorFontSize"), size);
    m_settings.sync();
}

bool AppSettings::editorWordWrap() const
{
    return m_settings.value(QStringLiteral("editorWordWrap"), false).toBool();
}

void AppSettings::setEditorWordWrap(bool enabled)
{
    m_settings.setValue(QStringLiteral("editorWordWrap"), enabled);
    m_settings.sync();
}

SyncSettings AppSettings::syncSettings(const QString &notebookId) const
{
    SyncSettings result;
    if (notebookId.isEmpty())
        return result;
    m_settings.beginGroup(QStringLiteral("sync"));
    m_settings.beginGroup(notebookId);
    result.enabled = m_settings.value(QStringLiteral("enabled"), false).toBool();
    result.endpoint = m_settings.value(QStringLiteral("endpoint")).toString();
    result.remotePath = m_settings.value(QStringLiteral("remotePath"), QStringLiteral("Qoplin")).toString();
    result.userName = m_settings.value(QStringLiteral("userName")).toString();
    result.serverPassword = m_settings.value(QStringLiteral("serverPassword")).toString();
    result.masterPassword = m_settings.value(QStringLiteral("masterPassword")).toString();
    m_settings.endGroup();
    m_settings.endGroup();
    return result;
}

void AppSettings::setSyncSettings(const QString &notebookId, const SyncSettings &settings)
{
    if (notebookId.isEmpty())
        return;
    m_settings.beginGroup(QStringLiteral("sync"));
    m_settings.beginGroup(notebookId);
    m_settings.setValue(QStringLiteral("enabled"), settings.enabled);
    m_settings.setValue(QStringLiteral("endpoint"), settings.endpoint);
    m_settings.setValue(QStringLiteral("remotePath"), settings.remotePath);
    m_settings.setValue(QStringLiteral("userName"), settings.userName);
    m_settings.setValue(QStringLiteral("serverPassword"), settings.serverPassword);
    m_settings.setValue(QStringLiteral("masterPassword"), settings.masterPassword);
    m_settings.endGroup();
    m_settings.endGroup();
    m_settings.sync();
}
