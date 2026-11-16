#include "update-manager.h"
#include "update-package.h"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>
#include <QVersionNumber>
#include <obs-module.h>

namespace {
constexpr auto releaseUrl = "https://api.github.com/repos/GuiCraftBR-eu/MultistreamOBS/releases/latest";
constexpr auto downloadPrefix = "https://github.com/GuiCraftBR-eu/MultistreamOBS/releases/download/";
constexpr auto assetName = "multistream-obs-update-win64.json";
constexpr qint64 maxMetadata = 1024 * 1024;
constexpr qint64 maxPackage = 16 * 1024 * 1024;
const QRegularExpression stableVersion(QStringLiteral("^v?([0-9]+\\.[0-9]+\\.[0-9]+)$"));
const QRegularExpression sha256(QStringLiteral("^sha256:([0-9a-fA-F]{64})$"));
}

UpdateManager::UpdateManager(const QString &pluginDirectory, const QString &currentVersion,
                             QLabel *status, QObject *parent)
    : QObject(parent), network_(nullptr), directory_(pluginDirectory),
      currentVersion_(currentVersion), status_(status)
{
    // OBS does not bundle Qt's TLS backend. Use the matching Schannel plugin shipped here.
    QCoreApplication::addLibraryPath(directory_ + QStringLiteral("/qt-plugins"));
    network_ = new QNetworkAccessManager(this);
    QTimer::singleShot(15000, this, [this] { check(); });
    auto timer = new QTimer(this);
    timer->setInterval(12 * 60 * 60 * 1000);
    connect(timer, &QTimer::timeout, this, [this] { check(); });
    timer->start();
}

QNetworkReply *UpdateManager::get(const QString &url, qint64 maximumBytes)
{
    QNetworkRequest request{QUrl(url)};
    request.setRawHeader("User-Agent", "MultistreamOBS-updater");
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30000);
    auto reply = network_->get(request);
    connect(reply, &QNetworkReply::downloadProgress, reply, [reply, maximumBytes](qint64 received, qint64 total) {
        if (received > maximumBytes || total > maximumBytes) reply->abort();
    });
    return reply;
}

void UpdateManager::reportFailure(const QString &reason)
{
    blog(LOG_WARNING, "[multistream-obs] Automatic update failed: %s", reason.toUtf8().constData());
    status_->setText(obs_module_text("Update.Error"));
    status_->setToolTip(reason);
    busy_ = false;
}

void UpdateManager::check()
{
#if defined(_WIN32) && defined(_WIN64)
    if (busy_ || pendingRestart_) return;
    busy_ = true;
    auto reply = get(QString::fromLatin1(releaseUrl), maxMetadata);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const auto bytes = reply->readAll();
        const auto error = reply->error();
        const auto code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        if (code == 404) { busy_ = false; return; } // No release exists yet.
        if (error != QNetworkReply::NoError || code != 200 || bytes.size() > maxMetadata) {
            reportFailure(QStringLiteral("Unable to check GitHub releases.")); return;
        }
        const auto doc = QJsonDocument::fromJson(bytes);
        if (!doc.isObject()) { reportFailure(QStringLiteral("Invalid release information.")); return; }
        const auto root = doc.object();
        const auto match = stableVersion.match(root.value("tag_name").toString());
        if (!match.hasMatch() || root.value("prerelease").toBool() || root.value("draft").toBool()) {
            reportFailure(QStringLiteral("Unexpected release version.")); return;
        }
        const auto version = match.captured(1);
        if (QVersionNumber::fromString(version) <= QVersionNumber::fromString(currentVersion_)) {
            status_->clear(); busy_ = false; return;
        }
        if ((obs_get_version() >> 24) != 33) {
            reportFailure(QStringLiteral("This update is only verified for OBS Studio 33.")); return;
        }
        const auto assets = root.value("assets").toArray();
        for (const auto &asset : assets) {
            const auto entry = asset.toObject();
            if (entry.value("name").toString() != QLatin1String(assetName)) continue;
            const QString url = entry.value("browser_download_url").toString();
            const auto digest = sha256.match(entry.value("digest").toString());
            const double size = entry.value("size").toDouble();
            if (!url.startsWith(QLatin1String(downloadPrefix)) || !digest.hasMatch() || size <= 0 || size > maxPackage) {
                reportFailure(QStringLiteral("Release update asset is invalid.")); return;
            }
            download(version, url, QByteArray::fromHex(digest.captured(1).toLatin1()));
            return;
        }
        reportFailure(QStringLiteral("The release has no Windows update package."));
    });
#endif
}

void UpdateManager::download(const QString &version, const QString &url, const QByteArray &digest)
{
    auto reply = get(url, maxPackage);
    connect(reply, &QNetworkReply::finished, this, [this, reply, version, digest] {
        const auto bytes = reply->readAll();
        const auto error = reply->error();
        const auto code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        if (error != QNetworkReply::NoError || code != 200 || bytes.size() > maxPackage ||
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256) != digest) {
            reportFailure(QStringLiteral("Update download or SHA-256 verification failed.")); return;
        }
        QVector<UpdateFile> files;
        QString reason;
        if (!ParseUpdatePackage(bytes, version, &files, &reason) ||
            !ApplyUpdatePackage(directory_, files, &reason)) {
            reportFailure(reason); return;
        }
        pendingRestart_ = true;
        busy_ = false;
        status_->setText(QString::fromUtf8(obs_module_text("Update.Restart")).arg(version));
        status_->setToolTip(obs_module_text("Update.RestartTip"));
        blog(LOG_INFO, "[multistream-obs] Update %s installed; restart OBS to use it.", version.toUtf8().constData());
    });
}
