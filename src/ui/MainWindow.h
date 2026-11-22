#pragma once

#include "models/NoteNode.h"

#include <QMainWindow>
#include <memory>

class AppSettings;
class NotebookStore;
class QThread;
class QTimer;
class QTreeWidgetItem;
class StatusBarContent;
class SyncWorker;
namespace Ui { class MainWindow; }

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    MainWindow(NotebookStore *store, AppSettings *settings, QWidget *parent = nullptr);
    ~MainWindow() override;

signals:
    void backRequested();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void rebuildTree(const QString &selectId = QString());
    QTreeWidgetItem *createTreeItem(const NoteNode &node);
    QTreeWidgetItem *findTreeItem(const QString &id) const;
    QString selectedNodeId() const;
    QString selectedParentId() const;
    void openSelectedNode();
    void clearEditor();
    void loadNote(const QString &id);
    enum class UnsavedAction { Save, Discard, Cancel };
    UnsavedAction resolveUnsavedChanges();
    void setDirty(bool dirty);
    bool saveCurrentNote();
    void createNode(NoteNode::Type type);
    void renameSelected();
    void deleteSelected();
    void applyTreeMoves();
    void filterTree(const QString &text);
    bool filterItem(QTreeWidgetItem *item, const QString &text);
    void setMode(int mode);
    void updatePreview();
    void setEditorAvailable(bool available);
    void openSettings();
    void openSyncDialog();
    void synchronizeNow();
    void startSync(const QString &serverPassword, const QString &masterPassword);
    void syncFinished(bool success, const QString &message, int uploaded, int downloaded);
    bool canLeaveEditor();

    Ui::MainWindow *ui;
    std::unique_ptr<NotebookStore> m_store;
    AppSettings *m_settings;
    StatusBarContent *m_statusContent = nullptr;
    QTimer *m_previewTimer = nullptr;
    QString m_currentNoteId;
    bool m_dirty = false;
    bool m_loading = false;
    bool m_rebuilding = false;
    QThread *m_syncThread = nullptr;
    SyncWorker *m_syncWorker = nullptr;
};
