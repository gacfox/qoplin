#pragma once

#include "models/NotebookInfo.h"

#include <QList>
#include <QSettings>
#include <QString>

struct SyncSettings
{
    bool enabled = false;
    QString endpoint;
    QString remotePath = QStringLiteral("Qoplin");
    QString userName;
    QString serverPassword;
    QString masterPassword;
};

class AppSettings
{
public:
    AppSettings();

    QList<NotebookInfo> notebooks() const;
    void registerNotebook(const NotebookInfo &info);
    void unregisterNotebook(const QString &rootPath);
    void renameNotebook(const QString &rootPath, const QString &name);
    void touchNotebook(const QString &rootPath);
    void removeMissingNotebooks();

    bool autoOpenLast() const;
    void setAutoOpenLast(bool value);
    QString lastNotebook() const;
    void setLastNotebook(const QString &rootPath);
    QString deviceId() const;

    QByteArray windowGeometry() const;
    void setWindowGeometry(const QByteArray &geometry);
    QByteArray splitterState() const;
    void setSplitterState(const QByteArray &state);
    int editorMode() const;
    void setEditorMode(int mode);
    QString editorFontFamily() const;
    void setEditorFontFamily(const QString &family);
    int editorFontSize() const;
    void setEditorFontSize(int size);
    bool editorWordWrap() const;
    void setEditorWordWrap(bool enabled);
    SyncSettings syncSettings(const QString &notebookId) const;
    void setSyncSettings(const QString &notebookId, const SyncSettings &settings);

private:
    mutable QSettings m_settings;
};
