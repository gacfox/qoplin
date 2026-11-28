#include "NotePlainTextEdit.h"

#include <QFileInfo>
#include <QMimeData>
#include <QUrl>

NotePlainTextEdit::NotePlainTextEdit(QWidget *parent) : QPlainTextEdit(parent) {}

void NotePlainTextEdit::insertFromMimeData(const QMimeData *source)
{
    if (source && source->hasUrls()) {
        const auto urls = source->urls();
        static const QStringList extensions{QStringLiteral("png"), QStringLiteral("jpg"),
            QStringLiteral("jpeg"), QStringLiteral("gif"), QStringLiteral("bmp"), QStringLiteral("webp")};
        bool allImageFiles = !urls.isEmpty();
        for (const QUrl &url : urls) {
            if (!url.isLocalFile() || !extensions.contains(QFileInfo(url.toLocalFile()).suffix().toLower())) {
                allImageFiles = false;
                break;
            }
        }
        if (allImageFiles) {
            emit imagePasteRequested(source);
            return;
        }
    }
    if (source && source->hasImage()) {
        emit imagePasteRequested(source);
        return;
    }
    QPlainTextEdit::insertFromMimeData(source);
}
