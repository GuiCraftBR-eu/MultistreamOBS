#pragma once

#include <QObject>
#include <QString>

class QLabel;
class QNetworkAccessManager;
class QNetworkReply;

// Checks the official GitHub release feed and installs verified files for the next OBS launch.
class UpdateManager final : public QObject {
public:
    UpdateManager(const QString &pluginDirectory, const QString &currentVersion, QLabel *status, QObject *parent);

private:
    void check();
    void download(const QString &version, const QString &url, const QByteArray &digest);
    void reportFailure(const QString &reason);
    QNetworkReply *get(const QString &url, qint64 maximumBytes);
    QNetworkAccessManager *network_;
    QString directory_;
    QString currentVersion_;
    QLabel *status_;
    bool busy_ = false;
    bool pendingRestart_ = false;
};
