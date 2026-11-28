#include "MainWindow.h"
#include "ui_MainWindow.h"
#include "storage/NotebookStore.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QMimeData>
#include <QMimeDatabase>
#include <QPixmap>
#include <QSaveFile>
#include <QTimer>
#include <QUrl>
#include <QUuid>

namespace {
constexpr qint64 kMaximumImageBytes = 25 * 1024 * 1024;
constexpr qint64 kMaximumImagePixels = 200000000;
constexpr int kValidationDecodeSize = 4096;

bool isSupportedExtension(const QString &extension)
{
    static const QStringList supported{QStringLiteral("png"), QStringLiteral("jpg"),
        QStringLiteral("jpeg"), QStringLiteral("gif"), QStringLiteral("bmp"), QStringLiteral("webp")};
    return supported.contains(extension.toLower());
}

QString escapedAltText(QString text)
{
    if (text.trimmed().isEmpty()) text = QStringLiteral("图片");
    text.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    text.replace(QLatin1Char('['), QStringLiteral("\\["));
    text.replace(QLatin1Char(']'), QStringLiteral("\\]"));
    return text;
}
}

void MainWindow::insertImagesFromFiles()
{
    if (m_currentNoteId.isEmpty()) return;
    const QStringList paths = QFileDialog::getOpenFileNames(this, tr("插入图片"), QString(),
        tr("图片文件 (*.png *.jpg *.jpeg *.gif *.bmp *.webp)"));
    for (const QString &path : paths) insertImageFile(path);
}

void MainWindow::insertImageFromClipboard(const QMimeData *mimeData)
{
    if (!mimeData || m_currentNoteId.isEmpty()) return;
    if (mimeData->hasImage()) {
        QImage image = qvariant_cast<QImage>(mimeData->imageData());
        if (image.isNull()) image = qvariant_cast<QPixmap>(mimeData->imageData()).toImage();
        if (image.isNull()) return;
        QByteArray bytes;
        QBuffer buffer(&bytes);
        if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) return;
        insertImageData(bytes, QStringLiteral("image/png"), QStringLiteral("png"), tr("剪贴板图片"));
        return;
    }
    if (mimeData->hasUrls()) {
        const auto urls = mimeData->urls();
        for (const QUrl &url : urls) {
            if (url.isLocalFile()) insertImageFile(url.toLocalFile());
        }
    }
}

bool MainWindow::insertImageFile(const QString &path)
{
    QFileInfo info(path);
    if (!info.isFile() || info.size() <= 0 || info.size() > kMaximumImageBytes) {
        ui->statusBar->showMessage(tr("图片不存在、为空或超过 25 MB：%1").arg(path), 6000);
        return false;
    }
    const QString extension = info.suffix().toLower();
    if (!isSupportedExtension(extension)) {
        ui->statusBar->showMessage(tr("不支持的图片格式：%1").arg(path), 6000);
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        ui->statusBar->showMessage(tr("无法读取图片：%1").arg(path), 6000);
        return false;
    }
    const QString mimeType = QMimeDatabase().mimeTypeForFile(path, QMimeDatabase::MatchExtension).name();
    return insertImageData(file.readAll(), mimeType, extension, info.fileName());
}

bool MainWindow::insertImageData(const QByteArray &data, const QString &mimeType,
                                 const QString &extension, const QString &originalName)
{
    if (m_currentNoteId.isEmpty() || data.isEmpty() || data.size() > kMaximumImageBytes) return false;
    if (!isSupportedExtension(extension)) return false;
    QBuffer input;
    input.setData(data);
    if (!input.open(QIODevice::ReadOnly)) return false;
    QImageReader reader(&input);
    const QSize dimensions = reader.size();
    if (dimensions.isEmpty() || static_cast<qint64>(dimensions.width()) * dimensions.height() > kMaximumImagePixels) {
        ui->statusBar->showMessage(tr("图片无法解码或尺寸过大。"), 6000);
        return false;
    }
    if (dimensions.width() > kValidationDecodeSize || dimensions.height() > kValidationDecodeSize)
        reader.setScaledSize(dimensions.scaled(kValidationDecodeSize, kValidationDecodeSize, Qt::KeepAspectRatio));
    const QImage image = reader.read();
    if (image.isNull()) {
        ui->statusBar->showMessage(tr("图片无法解码或尺寸过大。"), 6000);
        return false;
    }

    PendingAsset pending;
    pending.record.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
    pending.record.mimeType = mimeType.isEmpty() ? QStringLiteral("application/octet-stream") : mimeType;
    pending.record.extension = extension.toLower();
    pending.record.originalName = originalName;
    pending.record.size = data.size();
    pending.data = data;
    const QString id = pending.record.id;
    m_pendingAssets.insert(id, pending);

    const QString link = QStringLiteral("![%1](assets/%2.%3)\n")
        .arg(escapedAltText(originalName), id, pending.record.extension);
    ui->editor->textCursor().insertText(link);
    setDirty(true);
    m_previewTimer->start();
    return true;
}

QByteArray MainWindow::loadAssetData(const QString &path)
{
    const QString id = QFileInfo(path).baseName();
    const auto pending = m_pendingAssets.constFind(id);
    if (pending != m_pendingAssets.cend()) return pending->data;
    QString error;
    return m_store->readAsset(id, &error);
}

void MainWindow::exportAsset(const QString &path)
{
    const QString id = QFileInfo(path).baseName();
    const QByteArray data = loadAssetData(path);
    if (data.isEmpty()) {
        ui->statusBar->showMessage(tr("无法读取原图。"), 6000);
        return;
    }

    QString originalName;
    const auto pending = m_pendingAssets.constFind(id);
    if (pending != m_pendingAssets.cend()) originalName = pending->record.originalName;
    if (originalName.isEmpty()) originalName = m_store->asset(id).originalName;
    if (originalName.isEmpty()) originalName = tr("图片-%1.%2").arg(id.left(8), QFileInfo(path).suffix());

    QFileDialog dialog(this);
    dialog.setWindowTitle(tr("原图另存为"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setFileMode(QFileDialog::AnyFile);
    dialog.setOption(QFileDialog::DontUseNativeDialog, true);
    dialog.setDefaultSuffix(QFileInfo(path).suffix());
    dialog.selectFile(QDir::home().filePath(originalName));
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) return;

    QSaveFile output(dialog.selectedFiles().constFirst());
    if (!output.open(QIODevice::WriteOnly) || output.write(data) != data.size() || !output.commit()) {
        ui->statusBar->showMessage(tr("导出原图失败：%1").arg(output.errorString()), 8000);
        return;
    }
    ui->statusBar->showMessage(tr("原图已导出。"), 5000);
}

bool MainWindow::savePendingAssets(const QString &markdown, QString *error)
{
    if (m_pendingAssets.isEmpty()) return true;
    for (auto it = m_pendingAssets.cbegin(); it != m_pendingAssets.cend(); ++it) {
        const PendingAsset &pending = it.value();
        const QString reference = QStringLiteral("assets/%1.%2").arg(pending.record.id, pending.record.extension);
        if (!markdown.contains(reference)) continue;
        if (!m_store->writeAsset(pending.record, pending.data, error)) return false;
    }
    m_pendingAssets.clear();
    return true;
}
