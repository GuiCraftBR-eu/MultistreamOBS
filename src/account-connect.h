#pragma once
#include <QString>
#include <functional>
class QWidget;

// Imports the authorized account's current ingest credentials; OAuth tokens are not persisted.
void ConnectPlatformAccount(const QString &platform, QWidget *parent,
                            std::function<void(QString server, QString key, QString account)> finished);
