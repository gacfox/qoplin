#include "MainWindow.h"
#include "ui_MainWindow.h"
#include "SettingsDialog.h"
#include "StatusBarContent.h"
#include "storage/AppSettings.h"
#include "storage/NotebookStore.h"

#include <QAbstractItemModel>
#include <QActionGroup>
#include <QCloseEvent>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QTextBlock>
#include <QTextCursor>
#include <QTimer>
#include <QTreeWidgetItem>

#include <algorithm>
#include <functional>

namespace {
constexpr int IdRole = Qt::UserRole;
constexpr int TypeRole = Qt::UserRole + 1;
constexpr int ParentRole = Qt::UserRole + 2;

void applyPreviewCodeStyle(QTextDocument *document)
{
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        if (!block.blockFormat().hasProperty(QTextFormat::BlockCodeFence)) continue;
        QTextBlockFormat format = block.blockFormat();
        format.setBackground(QColor(QStringLiteral("#f2f2f2")));
        QTextCursor cursor(block);
        cursor.setBlockFormat(format);
    }
}
}

MainWindow::MainWindow(NotebookStore *store, AppSettings *settings, QWidget *parent)
    : QMainWindow(parent), ui(new Ui::MainWindow), m_store(store), m_settings(settings)
{
    ui->setupUi(this);
    // QMainWindow reapplies the active style's toolbar defaults during setupUi().
    ui->toolBar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    ui->toolBar->setIconSize(QSize(16, 16));
    setWindowTitle(QStringLiteral("%1[*] — Qoplin").arg(m_store->name()));
    ui->mainSplitter->setSizes({240, 760});
    ui->editorSplitter->setSizes({480, 480});
    if (!m_settings->windowGeometry().isEmpty()) restoreGeometry(m_settings->windowGeometry());
    if (!m_settings->splitterState().isEmpty()) ui->mainSplitter->restoreState(m_settings->splitterState());

    QFont editorFont(m_settings->editorFontFamily());
    editorFont.setPointSize(m_settings->editorFontSize());
    editorFont.setStyleHint(QFont::Monospace);
    ui->editor->setFont(editorFont);
    ui->editor->setLineWrapMode(m_settings->editorWordWrap()
        ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
    ui->actionWordWrap->setChecked(m_settings->editorWordWrap());
    connect(ui->actionWordWrap, &QAction::toggled, this, [this](bool enabled) {
        ui->editor->setLineWrapMode(enabled ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
        m_settings->setEditorWordWrap(enabled);
    });

    m_statusContent = new StatusBarContent(ui->statusBar);
    ui->statusBar->addWidget(m_statusContent, 1);

    m_previewTimer = new QTimer(this);
    m_previewTimer->setSingleShot(true);
    m_previewTimer->setInterval(250);
    connect(m_previewTimer, &QTimer::timeout, this, &MainWindow::updatePreview);

    connect(ui->noteTree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *, QTreeWidgetItem *) {
        if (!m_rebuilding) openSelectedNode();
    });
    connect(ui->noteTree->model(), &QAbstractItemModel::rowsMoved, this, [this] {
        if (!m_rebuilding) QTimer::singleShot(0, this, &MainWindow::applyTreeMoves);
    });
    connect(ui->editor, &QPlainTextEdit::textChanged, this, [this] {
        if (m_loading || m_currentNoteId.isEmpty()) return;
        setDirty(true);
        m_previewTimer->start();
    });
    connect(ui->noteTitleEdit, &QLineEdit::textEdited, this, [this] {
        if (!m_loading && !m_currentNoteId.isEmpty()) setDirty(true);
    });
    connect(ui->actionSave, &QAction::triggered, this, [this] { saveCurrentNote(); });
    connect(ui->actionExit, &QAction::triggered, this, &QMainWindow::close);

    ui->editor->addActions({ui->actionUndo, ui->actionRedo, ui->actionCut,
                            ui->actionCopy, ui->actionPaste, ui->actionSelectAll});
    connect(ui->actionUndo, &QAction::triggered, ui->editor, &QPlainTextEdit::undo);
    connect(ui->actionRedo, &QAction::triggered, ui->editor, &QPlainTextEdit::redo);
    connect(ui->actionCut, &QAction::triggered, ui->editor, &QPlainTextEdit::cut);
    connect(ui->actionCopy, &QAction::triggered, ui->editor, &QPlainTextEdit::copy);
    connect(ui->actionPaste, &QAction::triggered, ui->editor, &QPlainTextEdit::paste);
    connect(ui->actionSelectAll, &QAction::triggered, ui->editor, &QPlainTextEdit::selectAll);
    connect(ui->editor, &QPlainTextEdit::undoAvailable, this, [this](bool available) {
        ui->actionUndo->setEnabled(available && !m_currentNoteId.isEmpty());
    });
    connect(ui->editor, &QPlainTextEdit::redoAvailable, this, [this](bool available) {
        ui->actionRedo->setEnabled(available && !m_currentNoteId.isEmpty());
    });
    connect(ui->editor, &QPlainTextEdit::copyAvailable, this, [this](bool available) {
        const bool enabled = available && !m_currentNoteId.isEmpty();
        ui->actionCut->setEnabled(enabled);
        ui->actionCopy->setEnabled(enabled);
    });

    auto *modeGroup = new QActionGroup(this);
    modeGroup->setExclusive(true);
    modeGroup->addAction(ui->actionEditMode);
    modeGroup->addAction(ui->actionPreviewMode);
    modeGroup->addAction(ui->actionSplitMode);
    connect(ui->actionEditMode, &QAction::triggered, this, [this] { setMode(0); });
    connect(ui->actionPreviewMode, &QAction::triggered, this, [this] { setMode(1); });
    connect(ui->actionSplitMode, &QAction::triggered, this, [this] { setMode(2); });
    connect(ui->actionCycleView, &QAction::triggered, this, [this] {
        setMode((ui->modeCombo->currentIndex() + 1) % 3);
    });

    connect(ui->actionAbout, &QAction::triggered, this, [this] {
        QMessageBox::about(this, tr("关于 Qoplin"),
            tr("<h3>Qoplin 0.1.0</h3><p>轻量、本地优先的 Markdown 笔记。</p>"
               "<p>使用 C++、Qt Widgets 和 libsodium。</p>"));
    });
    connect(ui->filterEdit, &QLineEdit::textChanged, this, &MainWindow::filterTree);
    connect(ui->modeCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        setMode(index);
        m_settings->setEditorMode(index);
    });
    connect(ui->noteTree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QTreeWidgetItem *item = ui->noteTree->itemAt(pos);
        if (item) ui->noteTree->setCurrentItem(item);
        QMenu menu(this);
        menu.addAction(ui->actionNewNote);
        menu.addAction(ui->actionNewFolder);
        if (item) {
            menu.addSeparator();
            menu.addAction(ui->actionRename);
            menu.addAction(ui->actionDelete);
        }
        menu.exec(ui->noteTree->viewport()->mapToGlobal(pos));
    });

    connect(ui->actionBack, &QAction::triggered, this, [this] {
        if (canLeaveEditor()) emit backRequested();
    });
    connect(ui->actionNewNote, &QAction::triggered, this, [this] { createNode(NoteNode::Type::Note); });
    connect(ui->actionNewFolder, &QAction::triggered, this, [this] { createNode(NoteNode::Type::Folder); });
    connect(ui->actionRename, &QAction::triggered, this, &MainWindow::renameSelected);
    connect(ui->actionDelete, &QAction::triggered, this, &MainWindow::deleteSelected);
    connect(ui->actionSettings, &QAction::triggered, this, &MainWindow::openSettings);
    connect(ui->actionSyncSettings, &QAction::triggered, this, &MainWindow::openSyncDialog);
    connect(ui->actionSync, &QAction::triggered, this, &MainWindow::synchronizeNow);

    setEditorAvailable(false);
    setMode(m_settings->editorMode());
    rebuildTree();
}

MainWindow::~MainWindow() { delete ui; }

void MainWindow::rebuildTree(const QString &selectId)
{
    m_rebuilding = true;
    ui->noteTree->clear();
    QHash<QString, QTreeWidgetItem *> items;
    auto nodes = m_store->nodes();
    std::sort(nodes.begin(), nodes.end(), [](const NoteNode &a, const NoteNode &b) {
        if (a.sortOrder != b.sortOrder) return a.sortOrder < b.sortOrder;
        return QString::localeAwareCompare(a.title, b.title) < 0;
    });

    std::function<void(const QString &, QTreeWidgetItem *)> addChildren;
    addChildren = [&](const QString &parentId, QTreeWidgetItem *parentItem) {
        for (const NoteNode &node : nodes) {
            if (node.parentId != parentId) continue;
            auto *item = createTreeItem(node);
            if (parentItem) parentItem->addChild(item); else ui->noteTree->addTopLevelItem(item);
            items.insert(node.id, item);
            if (node.isFolder()) addChildren(node.id, item);
        }
    };
    addChildren(QString(), nullptr);
    ui->noteTree->expandAll();
    m_rebuilding = false;

    QString target = selectId;
    if (target.isEmpty() && !m_currentNoteId.isEmpty() && m_store->contains(m_currentNoteId)) target = m_currentNoteId;
    if (!target.isEmpty()) {
        if (auto *item = findTreeItem(target)) ui->noteTree->setCurrentItem(item);
    }
    filterTree(ui->filterEdit->text());
    if (!ui->noteTree->currentItem()) clearEditor();
}

QTreeWidgetItem *MainWindow::createTreeItem(const NoteNode &node)
{
    auto *item = new QTreeWidgetItem(QStringList(node.title));
    item->setData(0, IdRole, node.id);
    item->setData(0, TypeRole, node.isFolder() ? 1 : 0);
    item->setData(0, ParentRole, node.parentId);
    item->setFlags(item->flags() | Qt::ItemIsDragEnabled | (node.isFolder() ? Qt::ItemIsDropEnabled : Qt::NoItemFlags));
    item->setToolTip(0, node.isFolder() ? tr("文件夹") : tr("Markdown 笔记"));
    return item;
}

QTreeWidgetItem *MainWindow::findTreeItem(const QString &id) const
{
    std::function<QTreeWidgetItem *(QTreeWidgetItem *)> find = [&](QTreeWidgetItem *parent) -> QTreeWidgetItem * {
        const int count = parent ? parent->childCount() : ui->noteTree->topLevelItemCount();
        for (int i = 0; i < count; ++i) {
            auto *item = parent ? parent->child(i) : ui->noteTree->topLevelItem(i);
            if (item->data(0, IdRole).toString() == id) return item;
            if (auto *found = find(item)) return found;
        }
        return nullptr;
    };
    return find(nullptr);
}

QString MainWindow::selectedNodeId() const
{
    auto *item = ui->noteTree->currentItem();
    return item ? item->data(0, IdRole).toString() : QString();
}

QString MainWindow::selectedParentId() const
{
    auto *item = ui->noteTree->currentItem();
    if (!item) return QString();
    return item->data(0, TypeRole).toInt() == 1 ? item->data(0, IdRole).toString()
                                                : item->data(0, ParentRole).toString();
}

void MainWindow::openSelectedNode()
{
    if (m_dirty && resolveUnsavedChanges() == UnsavedAction::Cancel) {
        m_rebuilding = true;
        if (auto *item = findTreeItem(m_currentNoteId)) ui->noteTree->setCurrentItem(item);
        m_rebuilding = false;
        return;
    }
    const QString id = selectedNodeId();
    const NoteNode node = m_store->node(id);
    if (node.id.isEmpty() || node.deleted || !node.isNote()) clearEditor();
    else loadNote(id);
}

void MainWindow::clearEditor()
{
    m_loading = true;
    m_currentNoteId.clear();
    setDirty(false);
    ui->noteTitleEdit->clear();
    ui->editor->clear();
    ui->preview->clear();
    m_loading = false;
    setEditorAvailable(false);
    m_statusContent->setSaveText(tr("未选择笔记"));
}

void MainWindow::loadNote(const QString &id)
{
    const NoteNode node = m_store->node(id);
    QString error;
    const QString content = m_store->readNote(id, &error);
    if (!error.isEmpty()) {
        m_rebuilding = true;
        if (auto *item = findTreeItem(m_currentNoteId)) ui->noteTree->setCurrentItem(item);
        m_rebuilding = false;
        QMessageBox::critical(this, tr("打开笔记失败"), error);
        return;
    }
    m_loading = true;
    m_currentNoteId = id;
    setDirty(false);
    ui->noteTitleEdit->setText(node.title);
    ui->editor->setPlainText(content);
    ui->preview->setMarkdown(content);
    applyPreviewCodeStyle(ui->preview->document());
    m_loading = false;
    setEditorAvailable(true);
}

bool MainWindow::saveCurrentNote()
{
    if (m_currentNoteId.isEmpty()) return true;
    const QString title = ui->noteTitleEdit->text().trimmed();
    if (title.isEmpty()) {
        QMessageBox::warning(this, tr("保存笔记"), tr("笔记标题不能为空。"));
        ui->noteTitleEdit->setFocus();
        return false;
    }
    QString error;
    const NoteNode before = m_store->node(m_currentNoteId);
    if (before.title != title && !m_store->renameNode(m_currentNoteId, title, &error)) {
        QMessageBox::critical(this, tr("重命名失败"), error);
        return false;
    }
    if (m_dirty && !m_store->writeNote(m_currentNoteId, ui->editor->toPlainText(), &error)) {
        m_statusContent->setSaveText(tr("保存失败"));
        ui->statusBar->showMessage(error, 10000);
        return false;
    }
    setDirty(false);
    if (before.title != title) {
        if (auto *item = findTreeItem(m_currentNoteId)) item->setText(0, title);
    }
    return true;
}

void MainWindow::setDirty(bool dirty)
{
    m_dirty = dirty;
    setWindowModified(dirty);
    ui->actionSave->setEnabled(dirty && !m_currentNoteId.isEmpty());
    if (!m_currentNoteId.isEmpty())
        m_statusContent->setSaveText(dirty ? tr("未保存") : tr("已保存"));
}

MainWindow::UnsavedAction MainWindow::resolveUnsavedChanges()
{
    if (!m_dirty || m_currentNoteId.isEmpty()) return UnsavedAction::Save;
    const auto choice = QMessageBox::warning(this, tr("未保存的修改"),
        tr("笔记“%1”有未保存的修改。\n\n是否先保存？").arg(m_store->node(m_currentNoteId).title),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (choice == QMessageBox::Save)
        return saveCurrentNote() ? UnsavedAction::Save : UnsavedAction::Cancel;
    if (choice == QMessageBox::Discard) return UnsavedAction::Discard;
    return UnsavedAction::Cancel;
}

void MainWindow::createNode(NoteNode::Type type)
{
    const bool folder = type == NoteNode::Type::Folder;
    bool ok = false;
    const QString title = QInputDialog::getText(this, folder ? tr("新建文件夹") : tr("新建笔记"), tr("名称："),
        QLineEdit::Normal, folder ? tr("新建文件夹") : tr("未命名笔记"), &ok).trimmed();
    if (!ok || title.isEmpty()) return;
    const UnsavedAction unsavedAction = resolveUnsavedChanges();
    if (unsavedAction == UnsavedAction::Cancel) return;
    QString error;
    const QString id = m_store->createNode(type, selectedParentId(), title, &error);
    if (id.isEmpty()) { QMessageBox::critical(this, tr("创建失败"), error); return; }
    if (unsavedAction == UnsavedAction::Discard) setDirty(false);
    rebuildTree(id);
    if (!folder) ui->editor->setFocus();
}

void MainWindow::renameSelected()
{
    const QString id = selectedNodeId();
    if (id.isEmpty()) return;
    if (id == m_currentNoteId) { ui->noteTitleEdit->setFocus(); ui->noteTitleEdit->selectAll(); return; }
    const NoteNode node = m_store->node(id);
    bool ok = false;
    const QString title = QInputDialog::getText(this, tr("重命名"), tr("名称："), QLineEdit::Normal, node.title, &ok).trimmed();
    if (!ok || title.isEmpty() || title == node.title) return;
    QString error;
    if (!m_store->renameNode(id, title, &error)) { QMessageBox::critical(this, tr("重命名失败"), error); return; }
    rebuildTree(id);
}

void MainWindow::deleteSelected()
{
    const QString id = selectedNodeId();
    if (id.isEmpty()) return;
    if (resolveUnsavedChanges() == UnsavedAction::Cancel) return;
    const NoteNode node = m_store->node(id);
    const int count = m_store->subtreeIds(id).size();
    const QString detail = node.isFolder() && count > 1
        ? tr("文件夹“%1”及其包含的 %2 个项目将被删除。").arg(node.title).arg(count - 1)
        : tr("“%1”将被删除。").arg(node.title);
    if (QMessageBox::warning(this, tr("确认删除"), detail + tr("\n\n此操作会同步到其他设备。"),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
    QString error;
    if (!m_store->removeNode(id, &error)) { QMessageBox::critical(this, tr("删除失败"), error); return; }
    clearEditor();
    rebuildTree();
}

void MainWindow::applyTreeMoves()
{
    if (m_rebuilding) return;
    bool changed = false;
    std::function<bool(QTreeWidgetItem *)> visit = [&](QTreeWidgetItem *parent) -> bool {
        const int count = parent ? parent->childCount() : ui->noteTree->topLevelItemCount();
        for (int i = 0; i < count; ++i) {
            auto *item = parent ? parent->child(i) : ui->noteTree->topLevelItem(i);
            const QString id = item->data(0, IdRole).toString();
            const QString newParent = item->parent() ? item->parent()->data(0, IdRole).toString() : QString();
            const NoteNode oldNode = m_store->node(id);
            if (newParent != oldNode.parentId || oldNode.sortOrder != i) {
                QString error;
                if (!m_store->moveNode(id, newParent, i, &error)) {
                    QMessageBox::warning(this, tr("移动失败"), error);
                    rebuildTree(id);
                    return false;
                }
                item->setData(0, ParentRole, newParent);
                changed = true;
            }
            if (!visit(item)) return false;
        }
        return true;
    };
    if (visit(nullptr) && changed) ui->statusBar->showMessage(tr("目录位置与顺序已保存"), 2500);
}

void MainWindow::filterTree(const QString &text)
{
    for (int i = 0; i < ui->noteTree->topLevelItemCount(); ++i)
        filterItem(ui->noteTree->topLevelItem(i), text.trimmed());
}

bool MainWindow::filterItem(QTreeWidgetItem *item, const QString &text)
{
    bool childVisible = false;
    for (int i = 0; i < item->childCount(); ++i) childVisible |= filterItem(item->child(i), text);
    const bool matches = text.isEmpty() || item->text(0).contains(text, Qt::CaseInsensitive);
    const bool visible = matches || childVisible;
    item->setHidden(!visible);
    if (!text.isEmpty() && childVisible) item->setExpanded(true);
    return visible;
}

void MainWindow::setMode(int mode)
{
    mode = qBound(0, mode, 2);
    ui->actionEditMode->setChecked(mode == 0);
    ui->actionPreviewMode->setChecked(mode == 1);
    ui->actionSplitMode->setChecked(mode == 2);
    const QStringList modeNames{tr("编辑"), tr("预览"), tr("双列")};
    ui->actionCycleView->setToolTip(tr("视图切换（当前：%1）").arg(modeNames.at(mode)));
    if (ui->modeCombo->currentIndex() != mode) ui->modeCombo->setCurrentIndex(mode);
    ui->editor->setVisible(mode != 1);
    ui->preview->setVisible(mode != 0);
    if (mode != 0) updatePreview();
}

void MainWindow::updatePreview()
{
    if (ui->preview->isVisible()) {
        ui->preview->setMarkdown(ui->editor->toPlainText());
        applyPreviewCodeStyle(ui->preview->document());
    }
}

void MainWindow::setEditorAvailable(bool available)
{
    ui->noteTitleEdit->setEnabled(available);
    ui->editorSplitter->setEnabled(available);
    ui->actionSave->setEnabled(available && m_dirty);
    ui->actionUndo->setEnabled(available && ui->editor->document()->isUndoAvailable());
    ui->actionRedo->setEnabled(available && ui->editor->document()->isRedoAvailable());
    ui->actionCut->setEnabled(available && ui->editor->textCursor().hasSelection());
    ui->actionCopy->setEnabled(available && ui->editor->textCursor().hasSelection());
    ui->actionPaste->setEnabled(available && ui->editor->canPaste());
    ui->actionSelectAll->setEnabled(available);
}

void MainWindow::openSettings()
{
    SettingsDialog dialog(m_settings, this);
    if (dialog.exec() != QDialog::Accepted) return;
    QFont font(m_settings->editorFontFamily());
    font.setPointSize(m_settings->editorFontSize());
    font.setStyleHint(QFont::Monospace);
    ui->editor->setFont(font);
    setMode(m_settings->editorMode());
}

bool MainWindow::canLeaveEditor()
{
    if (m_syncThread) {
        QMessageBox::information(this, tr("同步进行中"), tr("请等待同步完成后再离开当前笔记本。"));
        return false;
    }
    const UnsavedAction action = resolveUnsavedChanges();
    if (action == UnsavedAction::Discard) setDirty(false);
    return action != UnsavedAction::Cancel;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!canLeaveEditor()) { event->ignore(); return; }
    m_settings->setWindowGeometry(saveGeometry());
    m_settings->setSplitterState(ui->mainSplitter->saveState());
    event->accept();
}
