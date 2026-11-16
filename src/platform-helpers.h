#pragma once

#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <cstdint>

bool IsKickTarget(const QString &platform, const QString &name);
QString NormalizeKickServer(const QString &server);
QStringList ObsResolutionOptions(uint32_t width, uint32_t height);

struct YouTubeStreamSettings {
    QString id;
    QString title;
    QString server;
    QString key;
    bool isDefault = false;
};
bool YouTubeStreamFromApi(const QJsonObject &item, YouTubeStreamSettings *out);

