#include "platform-helpers.h"
#include <QUrl>

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
