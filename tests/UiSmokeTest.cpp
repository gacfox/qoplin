#include "storage/AppSettings.h"
#include "storage/NotebookStore.h"
#include "ui/MainWindow.h"
#include "ui/MarkdownPreviewBrowser.h"
#include "ui/RemoteImportDialog.h"

#include <QAbstractButton>
#include <QAction>
#include <QClipboard>
#include <QComboBox>
#include <QFileDialog>
#include <QImage>
#include <QLabel>
#include <QLayout>
#include <QMimeData>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QtTest>

namespace {
int firstImageWidth(QTextDocument *document)
{
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (fragment.isValid() && fragment.charFormat().toImageFormat().isValid())
                return qRound(fragment.charFormat().toImageFormat().width());
        }
    }
    return 0;
}

void answerMessageBox(QMessageBox::StandardButton button)
{
    QTimer::singleShot(50, [button] {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        Q_ASSERT(box);
        Q_ASSERT(box->button(button));
        box->button(button)->click();
    });
}
}

class UiSmokeTest : public QObject
{
    Q_OBJECT
private slots:
    void editPreviewAndManualSave();
    void codeBlockHasGrayBackground();
    void remoteImportDialogIsCompact();
    void imagePasteSaveAndPreview();
    void largeJpegUsesScaledDecoding();
};

void UiSmokeTest::editPreviewAndManualSave()
{
    QCoreApplication::setOrganizationName(QStringLiteral("QoplinTests"));
    QCoreApplication::setApplicationName(QStringLiteral("UiSmokeTest"));
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    auto *store = new NotebookStore;
    QString error;
    QVERIFY2(store->create(temp.path(), QStringLiteral("UI Test"), QStringLiteral("test-device"), QStringLiteral("test master password"), &error), qPrintable(error));
    const QString folder = store->createNode(NoteNode::Type::Folder, QString(), QStringLiteral("Folder"), &error);
    const QString note = store->createNode(NoteNode::Type::Note, folder, QStringLiteral("Note"), &error);
    const QString secondNote = store->createNode(NoteNode::Type::Note, folder, QStringLiteral("Second"), &error);
    QVERIFY(!note.isEmpty() && !secondNote.isEmpty());

    AppSettings settings;
    settings.setEditorWordWrap(false);
    {
        MainWindow window(store, &settings);
        window.show();
        QTest::qWait(30);
        auto *tree = window.findChild<QTreeWidget *>(QStringLiteral("noteTree"));
        auto *editor = window.findChild<QPlainTextEdit *>(QStringLiteral("editor"));
        auto *preview = window.findChild<QTextBrowser *>(QStringLiteral("preview"));
        auto *mode = window.findChild<QComboBox *>(QStringLiteral("modeCombo"));
        auto *title = window.findChild<QLineEdit *>(QStringLiteral("noteTitleEdit"));
        auto *save = window.findChild<QAction *>(QStringLiteral("actionSave"));
        auto *splitMode = window.findChild<QAction *>(QStringLiteral("actionSplitMode"));
        auto *previewMode = window.findChild<QAction *>(QStringLiteral("actionPreviewMode"));
        auto *selectAll = window.findChild<QAction *>(QStringLiteral("actionSelectAll"));
        auto *copy = window.findChild<QAction *>(QStringLiteral("actionCopy"));
        auto *wordWrap = window.findChild<QAction *>(QStringLiteral("actionWordWrap"));
        auto *menuBar = window.findChild<QMenuBar *>(QStringLiteral("menuBar"));
        auto *toolBar = window.findChild<QToolBar *>(QStringLiteral("toolBar"));
        auto *newNote = window.findChild<QAction *>(QStringLiteral("actionNewNote"));
        auto *sync = window.findChild<QAction *>(QStringLiteral("actionSync"));
        auto *cycleView = window.findChild<QAction *>(QStringLiteral("actionCycleView"));
        auto *undo = window.findChild<QAction *>(QStringLiteral("actionUndo"));
        auto *redo = window.findChild<QAction *>(QStringLiteral("actionRedo"));
        auto *cut = window.findChild<QAction *>(QStringLiteral("actionCut"));
        auto *paste = window.findChild<QAction *>(QStringLiteral("actionPaste"));
        QVERIFY(tree && editor && preview && mode && title && save);
        QVERIFY(toolBar && newNote && sync);
        QCOMPARE(toolBar->toolButtonStyle(), Qt::ToolButtonIconOnly);
        QVERIFY(!newNote->icon().isNull());
        QVERIFY(!save->icon().isNull());
        QVERIFY(!sync->icon().isNull());
        QVERIFY(splitMode && previewMode && selectAll && copy && wordWrap && menuBar);
        QVERIFY(cycleView && undo && redo && cut && paste);
        for (QAction *action : {undo, redo, cut, copy, paste, cycleView, wordWrap})
            QVERIFY(!action->icon().isNull());
        QVERIFY(!wordWrap->isChecked());
        QCOMPARE(editor->lineWrapMode(), QPlainTextEdit::NoWrap);
        QCOMPARE(menuBar->actions().size(), 5);
        QCOMPARE(title->sizeHint().height(), mode->sizeHint().height());
        auto *saveLabel = window.findChild<QLabel *>(QStringLiteral("statusSaveLabel"));
        QVERIFY(saveLabel);
        QVERIFY(saveLabel->alignment() & Qt::AlignVCenter);
        QVERIFY(saveLabel->alignment() & Qt::AlignLeft);
        auto *statusContent = window.findChild<QWidget *>(QStringLiteral("StatusBarContent"));
        QVERIFY(statusContent);
        QVERIFY(statusContent->layout()->contentsMargins().left() >= 8);
        QVERIFY(!window.findChild<QLabel *>(QStringLiteral("emptyLabel")));
        QCOMPARE(tree->topLevelItemCount(), 1);
        QTreeWidgetItem *noteItem = tree->topLevelItem(0)->child(0);
        QTreeWidgetItem *secondItem = tree->topLevelItem(0)->child(1);
        QVERIFY(noteItem && secondItem);
        tree->setCurrentItem(noteItem);
        QCOMPARE(editor->toPlainText(), QString());
        QVERIFY(!save->isEnabled());
        wordWrap->trigger();
        QVERIFY(wordWrap->isChecked());
        QCOMPARE(editor->lineWrapMode(), QPlainTextEdit::WidgetWidth);
        QVERIFY(settings.editorWordWrap());

        editor->setPlainText(QStringLiteral("# Smoke Test\n\n**saved**"));
        QVERIFY(save->isEnabled());
        mode->setCurrentIndex(2);
        QTest::qWait(900);
        QVERIFY(splitMode->isChecked());
        selectAll->trigger();
        copy->trigger();
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("# Smoke Test\n\n**saved**"));
        previewMode->trigger();
        QCOMPARE(mode->currentIndex(), 1);
        QVERIFY(previewMode->isChecked());
        splitMode->trigger();
        QCOMPARE(mode->currentIndex(), 2);
        cycleView->trigger();
        QCOMPARE(mode->currentIndex(), 0);
        cycleView->trigger();
        QCOMPARE(mode->currentIndex(), 1);
        cycleView->trigger();
        QCOMPARE(mode->currentIndex(), 2);
        QVERIFY(cycleView->toolTip().contains(QStringLiteral("双列")));
        QVERIFY(preview->isVisible());
        QVERIFY(preview->document()->toPlainText().contains(QStringLiteral("Smoke Test")));
        QCOMPARE(store->readNote(note, &error), QString());
        QVERIFY2(error.isEmpty(), qPrintable(error));

        save->trigger();
        QCOMPARE(store->readNote(note, &error), QStringLiteral("# Smoke Test\n\n**saved**"));
        QVERIFY(!save->isEnabled());

        title->setFocus();
        title->selectAll();
        QTest::keyClicks(title, QStringLiteral("Renamed Note"));
        QVERIFY(save->isEnabled());
        save->trigger();
        QCOMPARE(store->node(note).title, QStringLiteral("Renamed Note"));

        editor->setPlainText(QStringLiteral("# Unsaved"));
        answerMessageBox(QMessageBox::Cancel);
        tree->setCurrentItem(secondItem);
        QCOMPARE(tree->currentItem(), noteItem);
        QCOMPARE(editor->toPlainText(), QStringLiteral("# Unsaved"));
        QVERIFY(save->isEnabled());
        QCOMPARE(store->readNote(note, &error), QStringLiteral("# Smoke Test\n\n**saved**"));

        answerMessageBox(QMessageBox::Discard);
        tree->setCurrentItem(secondItem);
        QCOMPARE(tree->currentItem(), secondItem);
        QCOMPARE(editor->toPlainText(), QString());
        QVERIFY(!save->isEnabled());

        tree->setCurrentItem(noteItem);
        QCOMPARE(editor->toPlainText(), QStringLiteral("# Smoke Test\n\n**saved**"));
        editor->setPlainText(QStringLiteral("# Saved on switch"));
        answerMessageBox(QMessageBox::Save);
        tree->setCurrentItem(secondItem);
        QCOMPARE(tree->currentItem(), secondItem);
        QCOMPARE(store->readNote(note, &error), QStringLiteral("# Saved on switch"));
        QVERIFY(!save->isEnabled());
        editor->setPlainText(QStringLiteral("Discard on close"));
        answerMessageBox(QMessageBox::Discard);
        window.close();
        QVERIFY(!window.isVisible());
        QCOMPARE(store->readNote(secondNote, &error), QString());
    }

    auto *reopenedStore = new NotebookStore;
    QVERIFY2(reopenedStore->open(temp.path(), QStringLiteral("test master password"), QStringLiteral("test-device"), &error), qPrintable(error));
    MainWindow restoredWindow(reopenedStore, &settings);
    auto *restoredEditor = restoredWindow.findChild<QPlainTextEdit *>(QStringLiteral("editor"));
    auto *restoredWordWrap = restoredWindow.findChild<QAction *>(QStringLiteral("actionWordWrap"));
    QVERIFY(restoredEditor && restoredWordWrap);
    QVERIFY(restoredWordWrap->isChecked());
    QCOMPARE(restoredEditor->lineWrapMode(), QPlainTextEdit::WidgetWidth);
}

void UiSmokeTest::codeBlockHasGrayBackground()
{
    QCoreApplication::setOrganizationName(QStringLiteral("QoplinTests"));
    QCoreApplication::setApplicationName(QStringLiteral("UiSmokeTest"));
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    auto *store = new NotebookStore;
    QString error;
    QVERIFY(store->create(temp.path(), QStringLiteral("Code Test"), QStringLiteral("test-device"), QStringLiteral("test master password"), &error));
    const QString note = store->createNode(NoteNode::Type::Note, QString(), QStringLiteral("Code"), &error);
    QVERIFY(store->writeNote(note, QStringLiteral("Before\n\n```cpp\nint value = 0;\n```\n"), &error));

    AppSettings settings;
    MainWindow window(store, &settings);
    auto *tree = window.findChild<QTreeWidget *>(QStringLiteral("noteTree"));
    auto *preview = window.findChild<QTextBrowser *>(QStringLiteral("preview"));
    QVERIFY(tree && preview);
    tree->setCurrentItem(tree->topLevelItem(0));

    bool foundCodeBlock = false;
    bool foundGrayBackground = false;
    for (QTextBlock block = preview->document()->begin(); block.isValid(); block = block.next()) {
        if (!block.blockFormat().hasProperty(QTextFormat::BlockCodeFence)) continue;
        foundCodeBlock = true;
        if (block.blockFormat().background().color() == QColor(QStringLiteral("#f2f2f2")))
            foundGrayBackground = true;
    }
    QVERIFY2(foundCodeBlock, "Markdown code block was not recognized");
    QVERIFY2(foundGrayBackground, qPrintable(preview->document()->toHtml()));
}

void UiSmokeTest::remoteImportDialogIsCompact()
{
    QCoreApplication::setOrganizationName(QStringLiteral("QoplinTests"));
    QCoreApplication::setApplicationName(QStringLiteral("UiSmokeTest"));
    AppSettings settings;
    RemoteImportDialog dialog(&settings);
    auto *message = dialog.findChild<QLabel *>(QStringLiteral("messageLabel"));
    QVERIFY(message && !message->isVisible());
    QVERIFY2(dialog.sizeHint().height() < 450,
             qPrintable(QStringLiteral("Unexpected dialog height: %1").arg(dialog.sizeHint().height())));
}

void UiSmokeTest::imagePasteSaveAndPreview()
{
    QCoreApplication::setOrganizationName(QStringLiteral("QoplinTests"));
    QCoreApplication::setApplicationName(QStringLiteral("UiSmokeTest"));
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    auto *store = new NotebookStore;
    QString error;
    QVERIFY2(store->create(temp.path(), QStringLiteral("Image Test"), QStringLiteral("test-device"),
                           QStringLiteral("test master password"), &error), qPrintable(error));
    const QString note = store->createNode(NoteNode::Type::Note, QString(), QStringLiteral("Images"), &error);
    QVERIFY(!note.isEmpty());

    AppSettings settings;
    MainWindow window(store, &settings);
    window.show();
    auto *tree = window.findChild<QTreeWidget *>(QStringLiteral("noteTree"));
    auto *editor = window.findChild<QPlainTextEdit *>(QStringLiteral("editor"));
    auto *preview = window.findChild<QTextBrowser *>(QStringLiteral("preview"));
    auto *save = window.findChild<QAction *>(QStringLiteral("actionSave"));
    auto *previewMode = window.findChild<QAction *>(QStringLiteral("actionPreviewMode"));
    QVERIFY(tree && editor && preview && save && previewMode);
    tree->setCurrentItem(tree->topLevelItem(0));

    QImage source(400, 240, QImage::Format_ARGB32);
    source.fill(QColor(220, 40, 60));
    auto *mimeData = new QMimeData;
    mimeData->setImageData(source);
    QApplication::clipboard()->setMimeData(mimeData);
    editor->paste();

    const QString markdown = editor->toPlainText();
    const QRegularExpressionMatch match = QRegularExpression(
        QStringLiteral("assets/([0-9a-f-]{36})\\.png")).match(markdown);
    QVERIFY(match.hasMatch());
    const QString assetId = match.captured(1);
    QCOMPARE(store->assets().size(), 0);

    save->trigger();
    QCOMPARE(store->assets().size(), 1);
    const QByteArray original = store->readAsset(assetId, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(original.startsWith(QByteArray("\x89PNG", 4)));
    QFile encryptedFile(store->assetPath(assetId));
    QVERIFY(encryptedFile.open(QIODevice::ReadOnly));
    const QByteArray encrypted = encryptedFile.readAll();
    QVERIFY(!encrypted.startsWith(QByteArray("\x89PNG", 4)));
    QVERIFY(!encrypted.contains(original));

    previewMode->trigger();
    const QVariant resource = preview->document()->resource(
        QTextDocument::ImageResource, QUrl(QStringLiteral("assets/%1.png").arg(assetId)));
    const QImage displayed = resource.value<QImage>();
    QVERIFY(!displayed.isNull());
    QCOMPARE(displayed.size(), source.size());

    MarkdownPreviewBrowser browser;
    browser.setAssetProvider([&original](const QString &) { return original; });
    browser.resize(200, 300);
    browser.show();
    browser.setMarkdown(markdown);
    QTest::qWait(50);
    QCOMPARE(firstImageWidth(browser.document()), browser.viewport()->width() - 24);
    browser.resize(300, 300);
    QTest::qWait(50);
    QCOMPARE(firstImageWidth(browser.document()), browser.viewport()->width() - 24);
    browser.setMarkdown(markdown);
    QTest::qWait(50);
    QCOMPARE(firstImageWidth(browser.document()), browser.viewport()->width() - 24);

    const QString exportPath = temp.path() + QStringLiteral("/exported.png");
    QString actualExportPath;
    QTimer::singleShot(100, [&exportPath, &actualExportPath] {
        auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
        Q_ASSERT(dialog);
        dialog->selectFile(exportPath);
        actualExportPath = dialog->selectedFiles().constFirst();
        QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
    });
    QMetaObject::invokeMethod(preview, "exportAssetRequested",
        Q_ARG(QString, QStringLiteral("assets/%1.png").arg(assetId)));
    QVERIFY(!actualExportPath.isEmpty());
    QFile exported(actualExportPath);
    QVERIFY(exported.open(QIODevice::ReadOnly));
    QCOMPARE(exported.readAll(), original);
}

void UiSmokeTest::largeJpegUsesScaledDecoding()
{
    const QString path = QStringLiteral("H:/workspace-web/3c2921c8ly1hvg4o58l48j272t3zcqvg.jpg");
    if (!QFileInfo::exists(path)) QSKIP("Local large JPEG fixture is unavailable");

    QCoreApplication::setOrganizationName(QStringLiteral("QoplinTests"));
    QCoreApplication::setApplicationName(QStringLiteral("UiSmokeTest"));
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    auto *store = new NotebookStore;
    QString error;
    QVERIFY(store->create(temp.path(), QStringLiteral("Large JPEG"), QStringLiteral("test-device"),
                          QStringLiteral("test master password"), &error));
    const QString note = store->createNode(NoteNode::Type::Note, QString(), QStringLiteral("Large"), &error);
    QVERIFY(!note.isEmpty());

    AppSettings settings;
    MainWindow window(store, &settings);
    auto *tree = window.findChild<QTreeWidget *>(QStringLiteral("noteTree"));
    auto *editor = window.findChild<QPlainTextEdit *>(QStringLiteral("editor"));
    auto *save = window.findChild<QAction *>(QStringLiteral("actionSave"));
    QVERIFY(tree && editor && save);
    tree->setCurrentItem(tree->topLevelItem(0));

    auto *mimeData = new QMimeData;
    mimeData->setUrls({QUrl::fromLocalFile(path)});
    QApplication::clipboard()->setMimeData(mimeData);
    editor->paste();
    QVERIFY(editor->toPlainText().contains(QStringLiteral("assets/")));
    save->trigger();
    QCOMPARE(store->assets().size(), 1);
    const QString assetId = store->assets().constFirst().id;
    QFile source(path);
    QVERIFY(source.open(QIODevice::ReadOnly));
    QCOMPARE(store->readAsset(assetId, &error), source.readAll());
}

QTEST_MAIN(UiSmokeTest)
#include "UiSmokeTest.moc"
