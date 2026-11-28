#include "MarkdownPreviewBrowser.h"

#include <QBuffer>
#include <QContextMenuEvent>
#include <QImage>
#include <QImageReader>
#include <QMenu>
#include <QRegularExpression>
#include <QTextCursor>
#include <QTextFragment>
#include <QTimer>

MarkdownPreviewBrowser::MarkdownPreviewBrowser(QWidget *parent) : QTextBrowser(parent) {}

void MarkdownPreviewBrowser::setAssetProvider(std::function<QByteArray(const QString &)> provider)
{
    m_assetProvider = std::move(provider);
}

void MarkdownPreviewBrowser::setMarkdown(const QString &markdown)
{
    m_originalImageSizes.clear();
    QTextBrowser::setMarkdown(markdown);
    QTimer::singleShot(0, this, &MarkdownPreviewBrowser::fitImagesToViewport);
}

QVariant MarkdownPreviewBrowser::loadResource(int type, const QUrl &name)
{
    if (type == QTextDocument::ImageResource && m_assetProvider) {
        const QString path = name.path();
        static const QRegularExpression assetPattern(
            QStringLiteral("^assets/[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\\.(png|jpe?g|gif|bmp|webp)$"),
            QRegularExpression::CaseInsensitiveOption);
        if (assetPattern.match(path).hasMatch()) {
            const QByteArray bytes = m_assetProvider(path);
            QBuffer input;
            input.setData(bytes);
            if (!input.open(QIODevice::ReadOnly)) return QImage();
            QImageReader reader(&input);
            const QSize originalSize = reader.size();
            if (!originalSize.isValid()) return QImage();
            m_originalImageSizes.insert(path, originalSize);
            constexpr int maximumDecodeSize = 2048;
            if (originalSize.width() > maximumDecodeSize || originalSize.height() > maximumDecodeSize)
                reader.setScaledSize(originalSize.scaled(maximumDecodeSize, maximumDecodeSize, Qt::KeepAspectRatio));
            return reader.read();
        }
        return QImage();
    }
    return QTextBrowser::loadResource(type, name);
}

void MarkdownPreviewBrowser::fitImagesToViewport()
{
    QList<QTextFragment> imageFragments;
    for (QTextBlock block = document()->begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (fragment.isValid() && fragment.charFormat().toImageFormat().isValid())
                imageFragments.append(fragment);
        }
    }

    const int availableWidth = qMax(1, viewport()->width() - 24);
    for (const QTextFragment &fragment : imageFragments) {
        QTextImageFormat format = fragment.charFormat().toImageFormat();
        const QSize original = m_originalImageSizes.value(format.name());
        if (!original.isValid() || original.width() <= 0 || original.height() <= 0) continue;
        const qreal scale = original.width() > availableWidth
            ? static_cast<qreal>(availableWidth) / original.width() : 1.0;
        const int width = qMax(1, qRound(original.width() * scale));
        const int height = qMax(1, qRound(original.height() * scale));
        if (qRound(format.width()) == width && qRound(format.height()) == height) continue;
        format.setWidth(width);
        format.setHeight(height);
        QTextCursor cursor(document());
        cursor.setPosition(fragment.position());
        cursor.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(format);
    }
}

void MarkdownPreviewBrowser::resizeEvent(QResizeEvent *event)
{
    QTextBrowser::resizeEvent(event);
    QTimer::singleShot(0, this, &MarkdownPreviewBrowser::fitImagesToViewport);
}

void MarkdownPreviewBrowser::contextMenuEvent(QContextMenuEvent *event)
{
    QTextCursor cursor = cursorForPosition(event->pos());
    QTextImageFormat imageFormat = cursor.charFormat().toImageFormat();
    if (!imageFormat.isValid()) {
        cursor.movePosition(QTextCursor::Left);
        imageFormat = cursor.charFormat().toImageFormat();
    }
    if (imageFormat.isValid() && !imageFormat.name().isEmpty()) {
        QMenu menu(this);
        menu.addAction(tr("原图另存为…"), [this, imageFormat] {
            emit exportAssetRequested(imageFormat.name());
        });
        menu.exec(event->globalPos());
        return;
    }
    QTextBrowser::contextMenuEvent(event);
}
