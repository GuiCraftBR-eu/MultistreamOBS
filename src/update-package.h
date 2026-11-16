#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

struct UpdateFile {
    QString path;
    QByteArray bytes;
};

// Parse and apply only the tightly scoped file format produced by package_update.py.
bool ParseUpdatePackage(const QByteArray &payload, const QString &version,
                        QVector<UpdateFile> *files, QString *error);
bool ApplyUpdatePackage(const QString &directory, const QVector<UpdateFile> &files,
                        QString *error);
