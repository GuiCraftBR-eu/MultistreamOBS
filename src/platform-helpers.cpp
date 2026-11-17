#include "platform-helpers.h"
#include <QHostAddress>
#include <QTcpServer>
#include <QUrl>

KickCallbackResult OpenKickCallback(QTcpServer *server, const QString &redirect)
{
    const QUrl url(redirect, QUrl::StrictMode);
    if (!server || !url.isValid() || url.scheme() != "http" ||
        url.host().compare(QStringLiteral("localhost"), Qt::CaseInsensitive) != 0 ||
        url.port() < 1 || url.port() > 65535 || !url.path().startsWith('/') ||
        !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment())
        return KickCallbackResult::InvalidUrl;
    return server->listen(QHostAddress::LocalHost, quint16(url.port()))
               ? KickCallbackResult::Listening : KickCallbackResult::PortUnavailable;
}

bool IsKickTarget(const QString &platform, const QString &name)
{
    return platform.compare(QStringLiteral("kick"), Qt::CaseInsensitive) == 0 ||
           (platform.isEmpty() && name.trimmed().compare(QStringLiteral("kick"), Qt::CaseInsensitive) == 0);
}

QString NormalizeKickServer(const QString &server)
{
    QUrl url(server.trimmed());
    const auto host = url.host().toLower();
    if (url.scheme().compare(QStringLiteral("rtmps"), Qt::CaseInsensitive) != 0 ||
        !host.endsWith(QStringLiteral(".global-contribute.live-video.net")) ||
        (url.path() != QString() && url.path() != QStringLiteral("/")) ||
        url.hasQuery() || url.hasFragment() || !url.userInfo().isEmpty())
        return server;
    url.setPath(QStringLiteral("/app"));
    if (url.port() == -1) url.setPort(443);
    return url.toString(QUrl::FullyEncoded);
}

QStringList ObsResolutionOptions(uint32_t width, uint32_t height)
{
    const double scales[] = {1.0, 1.25, 1.0 / 0.75, 1.5, 1.0 / 0.6, 1.75,
                             2.0, 2.25, 2.5, 2.75, 3.0};
    QStringList result;
    for (double scale : scales) {
        const auto w = uint32_t(double(width) / scale) & ~uint32_t(1);
        const auto h = uint32_t(double(height) / scale) & ~uint32_t(1);
        const auto value = QString::number(w) + "x" + QString::number(h);
        if (w && h && !result.contains(value)) result.append(value);
    }
    return result;
}

bool YouTubeStreamFromApi(const QJsonObject &item, YouTubeStreamSettings *out)
{
    if (!out) return false;
    const auto cdn = item.value("cdn").toObject();
    const auto ingestion = cdn.value("ingestionInfo").toObject();
    const auto server = ingestion.value("rtmpsIngestionAddress").toString().trimmed();
    const auto key = ingestion.value("streamName").toString().trimmed();
    const QUrl url(server);
    const auto host = url.host().toLower();
    if (cdn.value("ingestionType").toString() != "rtmp" || key.isEmpty() ||
        url.scheme() != "rtmps" || !host.endsWith(".youtube.com") || url.path().isEmpty() ||
        !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment() || server.contains(key))
        return false;
    const auto snippet = item.value("snippet").toObject();
    out->id = item.value("id").toString();
    out->title = snippet.value("title").toString().trimmed();
    if (out->title.isEmpty()) out->title = QStringLiteral("YouTube");
    out->server = server;
    out->key = key;
    out->isDefault = snippet.value("isDefaultStream").toBool();
    return true;
}

