#pragma once

#include <QString>
#include <QStringList>
#include <cstdint>

bool IsKickTarget(const QString &platform, const QString &name);
QString NormalizeKickServer(const QString &server);
QStringList ObsResolutionOptions(uint32_t width, uint32_t height);
