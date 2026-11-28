#pragma once

#include <QTextBrowser>

#include <QHash>
#include <QResizeEvent>
#include <functional>

class MarkdownPreviewBrowser : public QTextBrowser
{
    Q_OBJECT
public:
    explicit MarkdownPreviewBrowser(QWidget *parent = nullptr);
    void setAssetProvider(std::function<QByteArray(const QString &)> provider);
    void setMarkdown(const QString &markdown);

signals:
    void exportAssetRequested(const QString &assetPath);

public slots:
    void fitImagesToViewport();

protected:
    QVariant loadResource(int type, const QUrl &name) override;
    void resizeEvent(QResizeEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    std::function<QByteArray(const QString &)> m_assetProvider;
    QHash<QString, QSize> m_originalImageSizes;
};
