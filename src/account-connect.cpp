#include "account-connect.h"
#include "platform-helpers.h"
#include <obs-module.h>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHostAddress>
#include <QInputDialog>
#include <QMap>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSharedPointer>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrlQuery>
#include <QVBoxLayout>

namespace {
constexpr auto twitchClientId = "vbyiljlhljby8n1uwpjkpo4h5a6iiq";

QByteArray randomBytes(int count) {
    QByteArray bytes(count, 0);
    for (auto &c : bytes) c = char(QRandomGenerator::system()->generate() & 255);
    return bytes;
}
QString urlSafe(const QByteArray &bytes) {
    return QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

class AccountDialog final : public QDialog {
    QNetworkAccessManager network_{this};
    QTcpServer callback_{this};
    QTimer timer_{this};
    QLabel *status_;
    QLineEdit *code_;
    QPushButton *open_;
    QString platform_, deviceCode_, verificationUrl_, state_, verifier_, clientId_, clientSecret_, redirect_;
    QVector<YouTubeStreamSettings> youtubeStreams_;
    QSet<QString> youtubeKnownIds_;
    QMap<QString, QString> youtubeBroadcasts_;
    qint64 deadline_ = 0;
    int interval_ = 5;
    bool callbackUsed_ = false;
    std::function<void(QString, QString, QString)> finished_;

    void fail(const QString &message) {
        timer_.stop(); callback_.close();
        status_->setText(message);
        open_->setEnabled(false);
        deviceCode_.clear(); verifier_.clear(); state_.clear(); clientSecret_.clear();
    }

    void request(const QUrl &url, const QByteArray &method, const QUrlQuery &form,
                 const QList<QPair<QByteArray, QByteArray>> &headers,
                 std::function<void(QJsonObject, int, QNetworkReply::NetworkError)> done) {
        QNetworkRequest req(url);
        req.setRawHeader("User-Agent", "MultistreamOBS/1.2");
        req.setRawHeader("Accept", "application/json");
        req.setTransferTimeout(30000);
        // Credentials must never follow a redirect to another host.
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        for (const auto &h : headers) req.setRawHeader(h.first, h.second);
        QNetworkReply *reply;
        if (method == "POST") {
            req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
            QByteArray encoded;
            for (const auto &part : form.queryItems(QUrl::FullyDecoded)) {
                if (!encoded.isEmpty()) encoded += '&';
                encoded += QUrl::toPercentEncoding(part.first) + '=' + QUrl::toPercentEncoding(part.second);
            }
            reply = network_.post(req, encoded);
        } else reply = network_.get(req);
        connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 received, qint64 total) {
            if (received > 1024 * 1024 || total > 1024 * 1024) reply->abort();
        });
        connect(reply, &QNetworkReply::finished, this, [reply, done=std::move(done)] {
            const auto error = reply->error();
            const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const auto object = QJsonDocument::fromJson(reply->readAll()).object();
            reply->deleteLater();
            done(object, status, error);
        });
    }

    void beginTwitch() {
        QUrlQuery form;
        form.addQueryItem("client_id", twitchClientId);
        form.addQueryItem("scopes", "channel:read:stream_key");
        request(QUrl("https://id.twitch.tv/oauth2/device"), "POST", form, {},
                [this](QJsonObject json, int status, QNetworkReply::NetworkError error) {
            const auto uri = QUrl(json.value("verification_uri").toString());
            deviceCode_ = json.value("device_code").toString();
            if (error != QNetworkReply::NoError || status != 200 || deviceCode_.isEmpty() ||
                uri.scheme() != "https" || uri.host().compare("www.twitch.tv", Qt::CaseInsensitive) != 0) {
                fail(obs_module_text("Account.TwitchStartError")); return;
            }
            const int expires = json.value("expires_in").toInt(0);
            if (expires <= 0 || expires > 3600) { fail(obs_module_text("Account.TwitchStartError")); return; }
            deadline_ = QDateTime::currentSecsSinceEpoch() + expires;
            interval_ = qBound(5, json.value("interval").toInt(5), 60);
            verificationUrl_ = uri.toString();
            code_->setText(json.value("user_code").toString());
            code_->show(); open_->setEnabled(true);
            status_->setText(obs_module_text("Account.TwitchInstructions"));
            timer_.start(interval_ * 1000);
        });
    }

    void pollTwitch() {
        if (QDateTime::currentSecsSinceEpoch() >= deadline_) { fail(obs_module_text("Account.Expired")); return; }
        QUrlQuery form;
        form.addQueryItem("client_id", twitchClientId);
        form.addQueryItem("device_code", deviceCode_);
        form.addQueryItem("grant_type", "urn:ietf:params:oauth:grant-type:device_code");
        request(QUrl("https://id.twitch.tv/oauth2/token"), "POST", form, {},
                [this](QJsonObject json, int status, QNetworkReply::NetworkError error) {
            const auto kind = json.value("error").toString();
            const auto message = json.value("message").toString();
            if (kind == "authorization_pending" || message.contains("pending", Qt::CaseInsensitive)) {
                timer_.start(interval_ * 1000); return;
            }
            if (kind == "slow_down") { interval_ = qMin(interval_ + 5, 60); timer_.start(interval_ * 1000); return; }
            const auto token = json.value("access_token").toString();
            if (error != QNetworkReply::NoError || status != 200 || token.isEmpty()) {
                fail(obs_module_text("Account.SignInError")); return;
            }
            deviceCode_.clear();
            request(QUrl("https://id.twitch.tv/oauth2/validate"), "GET", {},
                    {{"Authorization", "OAuth " + token.toUtf8()}},
                    [this, token](QJsonObject detail, int code, QNetworkReply::NetworkError e) {
                if (e != QNetworkReply::NoError || code != 200 ||
                    detail.value("client_id").toString() != QLatin1String(twitchClientId) ||
                    detail.value("user_id").toString().isEmpty()) {
                    fail(obs_module_text("Account.SignInError")); return;
                }
                QUrl url("https://api.twitch.tv/helix/streams/key");
                QUrlQuery query; query.addQueryItem("broadcaster_id", detail.value("user_id").toString());
                url.setQuery(query);
                request(url, "GET", {}, {{"Client-Id", twitchClientId}, {"Authorization", "Bearer " + token.toUtf8()}},
                        [this, name=detail.value("login").toString()](QJsonObject response, int http, QNetworkReply::NetworkError err) {
                    const auto data = response.value("data").toArray();
                    const auto key = data.isEmpty() ? QString() : data.at(0).toObject().value("stream_key").toString();
                    if (err != QNetworkReply::NoError || http != 200 || key.isEmpty()) {
                        fail(obs_module_text("Account.StreamKeyError")); return;
                    }
                    finish({}, key, name);
                });
            });
        });
    }

    void beginKick() {
        bool ok = false;
        clientId_ = QInputDialog::getText(this, obs_module_text("Account.KickTitle"),
                                          obs_module_text("Account.KickClientId"), QLineEdit::Normal, {}, &ok).trimmed();
        if (!ok || clientId_.isEmpty()) { reject(); return; }
        clientSecret_ = QInputDialog::getText(this, obs_module_text("Account.KickTitle"),
                                              obs_module_text("Account.KickClientSecret"), QLineEdit::Password, {}, &ok);
        if (!ok || clientSecret_.isEmpty()) { reject(); return; }
        QString redirect = QStringLiteral("http://localhost:37365/callback");
        for (;;) {
            redirect = QInputDialog::getText(this, obs_module_text("Account.KickTitle"),
                                             obs_module_text("Account.KickRedirect"),
                                             QLineEdit::Normal, redirect, &ok).trimmed();
            if (!ok || redirect.isEmpty()) { reject(); return; }
            const auto result = OpenKickCallback(&callback_, redirect);
            if (result == KickCallbackResult::Listening) break;
            const auto text = result == KickCallbackResult::InvalidUrl
                                  ? QString::fromUtf8(obs_module_text("Account.InvalidKickRedirect"))
                                  : QString::fromUtf8(obs_module_text("Account.CallbackError")).arg(QUrl(redirect).port());
            QMessageBox::warning(this, obs_module_text("Account.KickTitle"), text);
        }
        redirect_ = redirect;
        verifier_ = urlSafe(randomBytes(64));
        state_ = urlSafe(randomBytes(32));
        const auto challenge = urlSafe(QCryptographicHash::hash(verifier_.toUtf8(), QCryptographicHash::Sha256));
        QUrl url("https://id.kick.com/oauth/authorize");
        QUrlQuery query;
        query.addQueryItem("client_id", clientId_); query.addQueryItem("redirect_uri", redirect_);
        query.addQueryItem("response_type", "code"); query.addQueryItem("scope", "channel:read streamkey:read");
        query.addQueryItem("state", state_); query.addQueryItem("code_challenge", challenge);
        query.addQueryItem("code_challenge_method", "S256"); url.setQuery(query);
        verificationUrl_ = url.toString();
        status_->setText(obs_module_text("Account.KickInstructions"));
        open_->setEnabled(true);
        timer_.start(180000);
        connect(&callback_, &QTcpServer::newConnection, this, [this] { receiveCallback(); });
        QDesktopServices::openUrl(url);
    }

    void beginYouTube() {
        // Offer the client file already chosen in YT-Chatterino, when available.
        // The user still selects the file; neither that file nor OAuth tokens are saved here.
        QString suggested;
        QFile settings(QDir::home().filePath(".ytchatterino.json"));
        if (settings.open(QIODevice::ReadOnly) && settings.size() <= 1024 * 1024)
            suggested = QJsonDocument::fromJson(settings.readAll()).object()
                            .value("features").toObject().value("google_client_file").toString();
        const auto path = QFileDialog::getOpenFileName(this, obs_module_text("Account.GoogleClientFile"),
                                                        suggested, QStringLiteral("JSON (*.json)"));
        if (path.isEmpty()) { reject(); return; }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) {
            fail(obs_module_text("Account.GoogleClientError")); return;
        }
        const auto client = QJsonDocument::fromJson(file.readAll()).object().value("installed").toObject();
        clientId_ = client.value("client_id").toString().trimmed();
        clientSecret_ = client.value("client_secret").toString();
        if (!clientId_.endsWith(".apps.googleusercontent.com")) {
            fail(obs_module_text("Account.GoogleClientError")); return;
        }
        if (!callback_.listen(QHostAddress::LocalHost, 0)) { fail(obs_module_text("Account.GoogleCallbackError")); return; }
        redirect_ = QStringLiteral("http://127.0.0.1:%1/").arg(callback_.serverPort());
        verifier_ = urlSafe(randomBytes(64));
        state_ = urlSafe(randomBytes(32));
        const auto challenge = urlSafe(QCryptographicHash::hash(verifier_.toUtf8(), QCryptographicHash::Sha256));
        QUrl url("https://accounts.google.com/o/oauth2/v2/auth");
        QUrlQuery query;
        query.addQueryItem("client_id", clientId_); query.addQueryItem("redirect_uri", redirect_);
        query.addQueryItem("response_type", "code");
        query.addQueryItem("scope", "https://www.googleapis.com/auth/youtube.readonly");
        query.addQueryItem("state", state_); query.addQueryItem("code_challenge", challenge);
        query.addQueryItem("code_challenge_method", "S256"); url.setQuery(query);
        verificationUrl_ = url.toString();
        status_->setText(obs_module_text("Account.GoogleInstructions"));
        open_->setEnabled(true);
        timer_.start(180000);
        connect(&callback_, &QTcpServer::newConnection, this, [this] { receiveCallback(); });
        QDesktopServices::openUrl(url);
    }

    void receiveCallback() {
        while (callback_.hasPendingConnections()) {
            auto socket = callback_.nextPendingConnection();
            auto buffer = QSharedPointer<QByteArray>::create();
            connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer] {
                buffer->append(socket->readAll());
                if (buffer->size() > 16384) { socket->disconnectFromHost(); return; }
                if (!buffer->contains("\r\n\r\n")) return;
                const auto first = buffer->split('\n').value(0).trimmed().split(' ');
                const QUrl target("http://localhost" + QString::fromUtf8(first.value(1)));
                const QUrlQuery query(target);
                const auto code = query.queryItemValue("code");
                const auto err = query.queryItemValue("error");
                const auto expectedPath = QUrl(redirect_).path();
                const bool valid = !callbackUsed_ && socket->peerAddress().isLoopback() && first.value(0) == "GET" &&
                    target.path() == expectedPath && !state_.isEmpty() && query.queryItemValue("state") == state_ &&
                    (!code.isEmpty() || !err.isEmpty());
                const QByteArray body = valid ? "Return to OBS to finish sign-in." : "Invalid sign-in callback.";
                socket->write(QByteArray(valid ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 400 Bad Request\r\n") +
                              "Content-Type: text/plain\r\nConnection: close\r\nContent-Length: " +
                              QByteArray::number(body.size()) + "\r\n\r\n" + body);
                socket->disconnectFromHost();
                if (!valid) return;
                callbackUsed_ = true; callback_.close(); timer_.stop();
                if (!err.isEmpty()) { fail(obs_module_text("Account.Denied")); return; }
                QUrlQuery form;
                form.addQueryItem("grant_type", "authorization_code"); form.addQueryItem("client_id", clientId_);
                if (!clientSecret_.isEmpty()) form.addQueryItem("client_secret", clientSecret_);
                form.addQueryItem("redirect_uri", redirect_);
                form.addQueryItem("code_verifier", verifier_); form.addQueryItem("code", code);
                clientSecret_.clear(); verifier_.clear(); state_.clear();
                const QUrl tokenUrl(platform_ == "youtube" ? "https://oauth2.googleapis.com/token" : "https://id.kick.com/oauth/token");
                request(tokenUrl, "POST", form, {},
                        [this](QJsonObject json, int http, QNetworkReply::NetworkError e) {
                    const auto token = json.value("access_token").toString();
                    if (e != QNetworkReply::NoError || http != 200 || token.isEmpty()) {
                        fail(obs_module_text("Account.SignInError")); return;
                    }
                    if (platform_ == "youtube") { fetchYouTubeStreams(token, {}, 0); return; }
                    request(QUrl("https://api.kick.com/public/v1/channels"), "GET", {},
                            {{"Authorization", "Bearer " + token.toUtf8()}},
                            [this](QJsonObject response, int status, QNetworkReply::NetworkError error) {
                        const auto data = response.value("data").toArray();
                        const auto channel = data.isEmpty() ? QJsonObject() : data.at(0).toObject();
                        const auto stream = channel.value("stream").toObject();
                        const auto key = stream.value("key").toString();
                        auto server = stream.value("url").toString();
                        // Some providers return the key appended to the URL. OBS logs the server,
                        // so strip it before saving to avoid duplication and credential leaks.
                        if (!key.isEmpty() && server.endsWith(QStringLiteral("/") + key)) server.chop(key.size() + 1);
                        const QUrl url(server);
                        if (error != QNetworkReply::NoError || status != 200 || key.isEmpty() ||
                            server.contains(key) || url.scheme() != "rtmps" || url.host().isEmpty() ||
                            !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()) {
                            fail(obs_module_text("Account.StreamKeyError")); return;
                        }
                        finish(server, key, channel.value("slug").toString());
                    });
                });
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    }

    void fetchYouTubeStreams(const QString &token, const QString &page, int pageCount) {
        QUrl url("https://www.googleapis.com/youtube/v3/liveStreams");
        QUrlQuery query;
        query.addQueryItem("part", "snippet,cdn");
        query.addQueryItem("mine", "true");
        query.addQueryItem("maxResults", "50");
        query.addQueryItem("fields", "nextPageToken,items(id,snippet(title,isDefaultStream),cdn(ingestionType,ingestionInfo(streamName,rtmpsIngestionAddress)))");
        if (!page.isEmpty()) query.addQueryItem("pageToken", page);
        url.setQuery(query);
        request(url, "GET", {}, {{"Authorization", "Bearer " + token.toUtf8()}},
                [this, token, pageCount](QJsonObject response, int status, QNetworkReply::NetworkError error) {
            if (error != QNetworkReply::NoError || status != 200) {
                fail(obs_module_text("Account.GoogleStreamsError")); return;
            }
            for (const auto &item : response.value("items").toArray()) {
                YouTubeStreamSettings stream;
                if (YouTubeStreamFromApi(item.toObject(), &stream) && !stream.id.isEmpty()) {
                    youtubeKnownIds_.insert(stream.id);
                    youtubeStreams_.append(stream);
                }
            }
            const auto next = response.value("nextPageToken").toString();
            if (!next.isEmpty() && pageCount < 19) { fetchYouTubeStreams(token, next, pageCount + 1); return; }
            // YouTube omits non-reusable streams from liveStreams.list(mine=true).
            // Look up bound streams of upcoming and active broadcasts separately.
            fetchYouTubeBroadcasts(token, "upcoming", {}, 0);
        });
    }

    void fetchYouTubeBroadcasts(const QString &token, const QString &phase, const QString &page, int pageCount) {
        QUrl url("https://www.googleapis.com/youtube/v3/liveBroadcasts");
        QUrlQuery query;
        query.addQueryItem("part", "snippet,contentDetails");
        query.addQueryItem("broadcastStatus", phase);
        query.addQueryItem("broadcastType", "all");
        query.addQueryItem("maxResults", "50");
        query.addQueryItem("fields", "nextPageToken,items(snippet(title),contentDetails(boundStreamId))");
        if (!page.isEmpty()) query.addQueryItem("pageToken", page);
        url.setQuery(query);
        request(url, "GET", {}, {{"Authorization", "Bearer " + token.toUtf8()}},
                [this, token, phase, pageCount](QJsonObject response, int status, QNetworkReply::NetworkError error) {
            if (error != QNetworkReply::NoError || status != 200) {
                if (youtubeStreams_.isEmpty()) fail(obs_module_text("Account.GoogleStreamsError"));
                else chooseYouTubeStream();
                return;
            }
            for (const auto &item : response.value("items").toArray()) {
                const auto object = item.toObject();
                const auto id = object.value("contentDetails").toObject().value("boundStreamId").toString();
                if (!id.isEmpty() && !youtubeKnownIds_.contains(id))
                    youtubeBroadcasts_.insert(id, object.value("snippet").toObject().value("title").toString());
            }
            const auto next = response.value("nextPageToken").toString();
            if (!next.isEmpty() && pageCount < 19) { fetchYouTubeBroadcasts(token, phase, next, pageCount + 1); return; }
            if (phase == "upcoming") { fetchYouTubeBroadcasts(token, "active", {}, 0); return; }
            fetchYouTubeBoundStreams(token, youtubeBroadcasts_.keys(), 0);
        });
    }

    void fetchYouTubeBoundStreams(const QString &token, const QStringList &ids, int offset) {
        if (offset >= ids.size()) { chooseYouTubeStream(); return; }
        QUrl url("https://www.googleapis.com/youtube/v3/liveStreams");
        QUrlQuery query;
        query.addQueryItem("part", "snippet,cdn");
        query.addQueryItem("id", ids.mid(offset, 50).join(','));
        query.addQueryItem("maxResults", "50");
        query.addQueryItem("fields", "items(id,snippet(title,isDefaultStream),cdn(ingestionType,ingestionInfo(streamName,rtmpsIngestionAddress)))");
        url.setQuery(query);
        request(url, "GET", {}, {{"Authorization", "Bearer " + token.toUtf8()}},
                [this, token, ids, offset](QJsonObject response, int status, QNetworkReply::NetworkError error) {
            if (error != QNetworkReply::NoError || status != 200) {
                if (youtubeStreams_.isEmpty()) fail(obs_module_text("Account.GoogleStreamsError"));
                else chooseYouTubeStream();
                return;
            }
            for (const auto &item : response.value("items").toArray()) {
                YouTubeStreamSettings stream;
                if (!YouTubeStreamFromApi(item.toObject(), &stream) || !youtubeBroadcasts_.contains(stream.id) ||
                    youtubeKnownIds_.contains(stream.id)) continue;
                const auto title = youtubeBroadcasts_.value(stream.id).trimmed();
                if (!title.isEmpty()) stream.title = title;
                youtubeKnownIds_.insert(stream.id);
                youtubeStreams_.append(stream);
            }
            fetchYouTubeBoundStreams(token, ids, offset + 50);
        });
    }

    void chooseYouTubeStream() {
        if (youtubeStreams_.isEmpty()) { fail(obs_module_text("Account.GoogleNoStreams")); return; }
        int selected = 0;
        if (youtubeStreams_.size() > 1) {
            QStringList names;
            int defaultIndex = 0;
            for (int i = 0; i < youtubeStreams_.size(); ++i) {
                const auto &stream = youtubeStreams_.at(i);
                names.append(QString::number(i + 1) + ". " + stream.title +
                             (stream.isDefault ? QStringLiteral(" (") + obs_module_text("YouTube.DefaultStream") + ")" : QString()));
                if (stream.isDefault) defaultIndex = i;
            }
            bool ok = false;
            const auto chosen = QInputDialog::getItem(this, obs_module_text("YouTube.SelectStream"),
                                                      obs_module_text("YouTube.SelectStreamHint"), names,
                                                      defaultIndex, false, &ok);
            if (!ok) { reject(); return; }
            selected = names.indexOf(chosen);
            if (selected < 0) { fail(obs_module_text("Account.GoogleStreamsError")); return; }
        }
        const auto stream = youtubeStreams_.at(selected);
        youtubeStreams_.clear();
        finish(stream.server, stream.key, stream.title);
    }

    void finish(const QString &server, const QString &key, const QString &account) {
        timer_.stop(); callback_.close();
        auto callback = std::move(finished_);
        if (callback) callback(server, key, account);
        accept();
    }

public:
    AccountDialog(QString platform, QWidget *parent, std::function<void(QString, QString, QString)> finished)
        : QDialog(parent), platform_(std::move(platform)), finished_(std::move(finished)) {
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowTitle(obs_module_text("Account.Title"));
        auto layout = new QVBoxLayout(this);
        status_ = new QLabel(obs_module_text("Account.Starting"), this); status_->setWordWrap(true);
        code_ = new QLineEdit(this); code_->setReadOnly(true); code_->setAlignment(Qt::AlignCenter); code_->hide();
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
        open_ = buttons->addButton(obs_module_text("Account.OpenBrowser"), QDialogButtonBox::ActionRole);
        open_->setEnabled(false);
        layout->addWidget(status_); layout->addWidget(code_); layout->addWidget(buttons);
        connect(open_, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl(verificationUrl_)); });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        timer_.setSingleShot(true);
        connect(&timer_, &QTimer::timeout, this, [this] {
            if (platform_ == "twitch") pollTwitch(); else fail(obs_module_text("Account.Expired"));
        });
        resize(480, 170);
        QTimer::singleShot(0, this, [this] {
            if (platform_ == "twitch") beginTwitch();
            else if (platform_ == "youtube") beginYouTube();
            else beginKick();
        });
    }
};
}

void ConnectPlatformAccount(const QString &platform, QWidget *parent,
                            std::function<void(QString, QString, QString)> finished)
{
    (new AccountDialog(platform.toLower(), parent, std::move(finished)))->open();
}

