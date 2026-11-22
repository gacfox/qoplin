#include "WebDavClient.h"

#include <QAuthenticator>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QStringDecoder>
#include <QTimer>
#include <QXmlStreamReader>

#include <algorithm>

namespace {

QByteArray basicAuthorization(const QString &userName, const QByteArray &password)
{
    QByteArray credentials = userName.toUtf8();
    credentials.append(':');
    credentials.append(password);
    const QByteArray result = QByteArrayLiteral("Basic ") + credentials.toBase64();
    std::fill(credentials.begin(), credentials.end(), '\0');
    return result;
}

QString statusMessage(const WebDavResponse &response)
{
    if (response.hasHttpStatus()) {
        const QString status = QStringLiteral("HTTP %1").arg(response.statusCode);
        return response.errorString.isEmpty()
            ? status : QStringLiteral("%1 (%2)").arg(status, response.errorString);
    }
    return response.errorString.isEmpty() ? QStringLiteral("network error") : response.errorString;
}

bool isHexDigit(char value)
{
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f')
        || (value >= 'A' && value <= 'F');
}

bool decodeUrlSegment(const QByteArray &encoded, QString *decoded)
{
    for (qsizetype index = 0; index < encoded.size(); ++index) {
        if (encoded.at(index) == '%'
            && (index + 2 >= encoded.size()
                || !isHexDigit(encoded.at(index + 1))
                || !isHexDigit(encoded.at(index + 2)))) {
            return false;
        }
    }

    const QByteArray bytes = QByteArray::fromPercentEncoding(encoded);
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString result = decoder.decode(bytes);
    if (decoder.hasError())
        return false;
    *decoded = result;
    return true;
}

bool decodedPathSegments(const QUrl &url, QStringList *segments)
{
    const QList<QByteArray> encodedSegments = url.path(QUrl::FullyEncoded)
        .toLatin1().split('/');
    segments->clear();
    for (const QByteArray &encoded : encodedSegments) {
        if (encoded.isEmpty())
            continue;
        QString decoded;
        if (!decodeUrlSegment(encoded, &decoded))
            return false;
        segments->append(decoded);
    }
    return true;
}

bool sameOrigin(const QUrl &left, const QUrl &right)
{
    return left.scheme().compare(right.scheme(), Qt::CaseInsensitive) == 0
        && left.host().compare(right.host(), Qt::CaseInsensitive) == 0
        && left.port() == right.port();
}

} // namespace

WebDavClient::WebDavClient(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
{
    connect(m_network, &QNetworkAccessManager::authenticationRequired,
            this, [this](QNetworkReply *, QAuthenticator *authenticator) {
                if (!authenticator || m_settings.userName.isEmpty())
                    return;
                authenticator->setUser(m_settings.userName);
                authenticator->setPassword(QString::fromUtf8(m_serverPassword));
            });
}

WebDavClient::~WebDavClient()
{
    if (!m_serverPassword.isEmpty())
        std::fill(m_serverPassword.begin(), m_serverPassword.end(), '\0');
    m_serverPassword.clear();
}

void WebDavClient::configure(const SyncSettings &settings, const QString &serverPassword)
{
    m_settings = settings;
    if (!m_serverPassword.isEmpty())
        std::fill(m_serverPassword.begin(), m_serverPassword.end(), '\0');
    m_serverPassword = serverPassword.toUtf8();
}

void WebDavClient::setTimeout(int milliseconds)
{
    m_timeoutMs = std::max(1, milliseconds);
}

void WebDavClient::cancel()
{
    m_cancelled.store(true);
}

void WebDavClient::resetCancellation()
{
    m_cancelled.store(false);
}

QUrl WebDavClient::endpointUrl(QString *error) const
{
    const QUrl url(m_settings.endpoint);
    if (!url.isValid() || (url.scheme() != QStringLiteral("http")
                           && url.scheme() != QStringLiteral("https"))
        || url.host().isEmpty()) {
        if (error)
            *error = QStringLiteral("The WebDAV endpoint must be a valid absolute HTTP(S) URL.");
        return {};
    }
    if (!url.userInfo().isEmpty()) {
        if (error)
            *error = QStringLiteral("Credentials must not be embedded in the WebDAV endpoint.");
        return {};
    }

    QUrl clean = url;
    clean.setQuery(QString());
    clean.setFragment(QString());
    if (error)
        error->clear();
    return clean;
}

bool WebDavClient::isConfigured(QString *error) const
{
    QUrl root = endpointUrl(error);
    if (!root.isValid())
        return false;
    if (!appendPath(root, m_settings.remotePath, &root)
        || !appendPath(root, QStringLiteral("header.json"), &root)) {
        if (error)
            *error = QStringLiteral("The WebDAV remote path is invalid or unsafe.");
        return false;
    }
    return true;
}

QStringList WebDavClient::pathSegments(const QString &path, bool *ok)
{
    *ok = false;
    if (path.contains(QLatin1Char('\\')) || path.contains(QChar::Null))
        return {};

    QStringList result;
    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        if (part == QStringLiteral(".") || part == QStringLiteral(".."))
            return {};
        result.append(part);
    }
    *ok = true;
    return result;
}

bool WebDavClient::appendPath(const QUrl &base, const QString &path, QUrl *result)
{
    if (!result || !base.isValid())
        return false;
    bool ok = false;
    const QStringList segments = pathSegments(path, &ok);
    if (!ok)
        return false;

    QString combined = base.path();
    if (!combined.isEmpty() && !combined.endsWith(QLatin1Char('/')))
        combined.append(QLatin1Char('/'));
    combined.append(segments.join(QLatin1Char('/')));
    if (combined.isEmpty())
        combined = QStringLiteral("/");

    *result = base;
    // DecodedMode prevents a literal "%2F" in a name from becoming a path slash.
    result->setPath(combined, QUrl::DecodedMode);
    result->setQuery(QString());
    result->setFragment(QString());
    return result->isValid();
}

QUrl WebDavClient::remoteUrl(const QString &relativePath) const
{
    QUrl root = endpointUrl();
    if (!root.isValid() || !appendPath(root, m_settings.remotePath, &root))
        return {};
    QUrl result;
    if (!appendPath(root, relativePath, &result))
        return {};
    return result;
}

WebDavResponse WebDavClient::request(const QByteArray &method, const QUrl &url,
                                     const QByteArray &data,
                                     const QByteArray &contentType,
                                     int depth)
{
    WebDavResponse response;
    if (m_cancelled.load()) {
        response.errorString = QStringLiteral("The request was cancelled.");
        return response;
    }
    if (!url.isValid() || (url.scheme() != QStringLiteral("http")
                           && url.scheme() != QStringLiteral("https"))) {
        response.errorString = QStringLiteral("The WebDAV request URL is invalid.");
        return response;
    }

    QNetworkRequest networkRequest(url);
    networkRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                QNetworkRequest::NoLessSafeRedirectPolicy);
    networkRequest.setTransferTimeout(m_timeoutMs);
    if (!m_settings.userName.isEmpty()) {
        const QByteArray authorization = basicAuthorization(m_settings.userName, m_serverPassword);
        networkRequest.setRawHeader(QByteArrayLiteral("Authorization"), authorization);
    }
    if (!contentType.isEmpty())
        networkRequest.setHeader(QNetworkRequest::ContentTypeHeader,
                                 QString::fromLatin1(contentType));
    if (method == QByteArrayLiteral("PROPFIND"))
        networkRequest.setRawHeader(QByteArrayLiteral("Depth"), QByteArray::number(depth));
    networkRequest.setRawHeader(QByteArrayLiteral("Accept"), QByteArrayLiteral("*/*"));

    QNetworkReply *reply = m_network->sendCustomRequest(networkRequest, method, data);
    if (!reply) {
        response.errorString = QStringLiteral("The WebDAV request could not be started.");
        return response;
    }

    QEventLoop loop;
    QTimer timeoutTimer;
    QTimer cancellationTimer;
    timeoutTimer.setSingleShot(true);
    timeoutTimer.setInterval(m_timeoutMs);
    cancellationTimer.setInterval(50);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(&cancellationTimer, &QTimer::timeout, &loop, [this, &loop]() {
        if (m_cancelled.load())
            loop.quit();
    });
    timeoutTimer.start();
    cancellationTimer.start();
    if (!reply->isFinished())
        loop.exec();

    const bool timedOut = !reply->isFinished() && !m_cancelled.load();
    if (!reply->isFinished())
        reply->abort();

    response.statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    response.body = reply->readAll();
    response.finalUrl = reply->url();
    if (m_cancelled.load())
        response.errorString = QStringLiteral("The request was cancelled.");
    else if (timedOut)
        response.errorString = QStringLiteral("The WebDAV request timed out.");
    else if (reply->error() != QNetworkReply::NoError)
        response.errorString = reply->errorString();
    reply->deleteLater();
    return response;
}

WebDavResponse WebDavClient::get(const QString &relativePath)
{
    return request(QByteArrayLiteral("GET"), remoteUrl(relativePath));
}

WebDavResponse WebDavClient::put(const QString &relativePath, const QByteArray &data)
{
    return request(QByteArrayLiteral("PUT"), remoteUrl(relativePath), data,
                   QByteArrayLiteral("application/octet-stream"));
}

WebDavResponse WebDavClient::propFind(const QString &relativePath)
{
    static const QByteArray body = QByteArrayLiteral(
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<d:propfind xmlns:d=\"DAV:\"><d:prop><d:resourcetype/>"
        "</d:prop></d:propfind>");
    return request(QByteArrayLiteral("PROPFIND"), remoteUrl(relativePath), body,
                   QByteArrayLiteral("application/xml; charset=utf-8"));
}

QStringList WebDavClient::listCollections(const QString &relativePath, QString *error)
{
    QStringList result;
    if (error)
        error->clear();

    static const QByteArray body = QByteArrayLiteral(
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<d:propfind xmlns:d=\"DAV:\"><d:prop><d:resourcetype/>"
        "</d:prop></d:propfind>");
    const QUrl requestedUrl = remoteUrl(relativePath);
    if (!requestedUrl.isValid()) {
        if (error)
            *error = QStringLiteral("The WebDAV collection path is invalid or unsafe.");
        return result;
    }

    const WebDavResponse response = request(QByteArrayLiteral("PROPFIND"), requestedUrl, body,
                                            QByteArrayLiteral("application/xml; charset=utf-8"), 1);
    if (!response.isSuccess()) {
        responseError(response, QStringLiteral("Listing remote collections"), error);
        return result;
    }

    QUrl baseUrl = response.finalUrl.isValid() ? response.finalUrl : requestedUrl;
    baseUrl.setQuery(QString());
    baseUrl.setFragment(QString());
    QStringList baseSegments;
    if (!decodedPathSegments(baseUrl, &baseSegments)) {
        if (error)
            *error = QStringLiteral("The WebDAV collection URL cannot be decoded safely.");
        return result;
    }

    QXmlStreamReader xml(response.body);
    QSet<QString> names;
    bool inResponse = false;
    bool inResourceType = false;
    bool collection = false;
    bool invalidHref = false;
    QString href;

    const auto processResponse = [&]() {
        if (!collection || href.trimmed().isEmpty())
            return;
        const QUrl parsedHref = QUrl::fromEncoded(href.trimmed().toUtf8(), QUrl::StrictMode);
        if (!parsedHref.isValid()) {
            invalidHref = true;
            return;
        }

        QUrl resolutionBase = baseUrl;
        QString basePath = resolutionBase.path();
        if (!basePath.endsWith(QLatin1Char('/')))
            basePath.append(QLatin1Char('/'));
        resolutionBase.setPath(basePath);
        const QUrl resolvedHref = resolutionBase.resolved(parsedHref);
        if (!sameOrigin(baseUrl, resolvedHref))
            return;

        QStringList childSegments;
        if (!decodedPathSegments(resolvedHref, &childSegments)) {
            invalidHref = true;
            return;
        }
        if (childSegments.size() != baseSegments.size() + 1)
            return;
        for (qsizetype index = 0; index < baseSegments.size(); ++index) {
            if (childSegments.at(index) != baseSegments.at(index))
                return;
        }

        const QString name = childSegments.constLast();
        // A decoded separator cannot be represented as one safe WebDAV path segment.
        if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral("..")
            || name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))
            || name.contains(QChar::Null)) {
            invalidHref = true;
            return;
        }
        names.insert(name);
    };

    while (!xml.atEnd()) {
        const QXmlStreamReader::TokenType token = xml.readNext();
        if (token == QXmlStreamReader::DTD) {
            xml.raiseError(QStringLiteral("DTD is not allowed"));
            break;
        }
        if (token == QXmlStreamReader::StartElement) {
            if (!inResponse && xml.namespaceUri() == QStringLiteral("DAV:")
                && xml.name() == QStringLiteral("response")) {
                inResponse = true;
                inResourceType = false;
                collection = false;
                href.clear();
            } else if (inResponse && xml.namespaceUri() == QStringLiteral("DAV:")
                       && xml.name() == QStringLiteral("href")) {
                href = xml.readElementText();
            } else if (inResponse && xml.namespaceUri() == QStringLiteral("DAV:")
                       && xml.name() == QStringLiteral("resourcetype")) {
                inResourceType = true;
            } else if (inResponse && inResourceType
                       && xml.namespaceUri() == QStringLiteral("DAV:")
                       && xml.name() == QStringLiteral("collection")) {
                collection = true;
            }
        } else if (token == QXmlStreamReader::EndElement && inResponse) {
            if (xml.namespaceUri() == QStringLiteral("DAV:")
                && xml.name() == QStringLiteral("resourcetype")) {
                inResourceType = false;
            } else if (xml.namespaceUri() == QStringLiteral("DAV:")
                       && xml.name() == QStringLiteral("response")) {
                processResponse();
                inResponse = false;
                inResourceType = false;
                collection = false;
                href.clear();
            }
        }
        if (invalidHref)
            break;
    }

    if (invalidHref || xml.hasError()) {
        if (error) {
            *error = invalidHref
                ? QStringLiteral("The WebDAV collection listing contains an invalid or unsafe URL.")
                : QStringLiteral("The WebDAV collection listing is not valid XML: %1 (line %2).")
                      .arg(xml.errorString()).arg(xml.lineNumber());
        }
        return {};
    }

    result = names.values();
    std::sort(result.begin(), result.end());
    return result;
}

WebDavResponse WebDavClient::mkCol(const QString &relativePath)
{
    return request(QByteArrayLiteral("MKCOL"), remoteUrl(relativePath));
}

bool WebDavClient::responseError(const WebDavResponse &response, const QString &action,
                                 QString *error)
{
    if (error) {
        *error = QStringLiteral("%1 failed: %2.").arg(action, statusMessage(response));
    }
    return false;
}

bool WebDavClient::ensureCollection(const QString &relativePath, QString *error)
{
    QUrl current = endpointUrl(error);
    if (!current.isValid())
        return false;

    bool ok = false;
    QStringList segments = pathSegments(m_settings.remotePath, &ok);
    if (!ok) {
        if (error)
            *error = QStringLiteral("The remote path contains an unsafe path segment.");
        return false;
    }
    const QStringList relativeSegments = pathSegments(relativePath, &ok);
    if (!ok) {
        if (error)
            *error = QStringLiteral("The remote collection path contains an unsafe path segment.");
        return false;
    }
    segments.append(relativeSegments);

    // Also verify the endpoint itself when no additional collection is configured.
    const qsizetype count = std::max<qsizetype>(1, segments.size());
    for (qsizetype index = 0; index < count; ++index) {
        if (m_cancelled.load()) {
            if (error)
                *error = QStringLiteral("Synchronization was cancelled.");
            return false;
        }
        if (index < segments.size() && !appendPath(current, segments.at(index), &current)) {
            if (error)
                *error = QStringLiteral("The remote collection URL could not be constructed.");
            return false;
        }

        static const QByteArray body = QByteArrayLiteral(
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
            "<d:propfind xmlns:d=\"DAV:\"><d:prop><d:resourcetype/>"
            "</d:prop></d:propfind>");
        WebDavResponse found = request(QByteArrayLiteral("PROPFIND"), current, body,
                                       QByteArrayLiteral("application/xml; charset=utf-8"));
        if (found.isSuccess())
            continue;
        if (found.statusCode != 404)
            return responseError(found, QStringLiteral("Checking the remote collection"), error);

        WebDavResponse created = request(QByteArrayLiteral("MKCOL"), current);
        if (created.isSuccess())
            continue;
        if (created.statusCode == 405) {
            // Some servers report 405 for an existing collection. Confirm it.
            found = request(QByteArrayLiteral("PROPFIND"), current, body,
                            QByteArrayLiteral("application/xml; charset=utf-8"));
            if (found.isSuccess())
                continue;
        }
        return responseError(created, QStringLiteral("Creating the remote collection"), error);
    }
    if (error)
        error->clear();
    return true;
}
