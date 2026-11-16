#include "update-package.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <algorithm>

namespace {
bool fail(QString *error, const QString &reason) { if (error) *error = reason; return false; }
constexpr qsizetype maxPackage = 16 * 1024 * 1024;
constexpr qsizetype maxFile = 8 * 1024 * 1024;
const QRegularExpression localePath(QStringLiteral("^data/locale/[A-Za-z0-9]+(?:-[A-Za-z0-9]+)*\\.ini$"));
const QString tlsPath = QStringLiteral("qt-plugins/tls/qschannelbackend.dll");

bool plainDir(const QString &path) {
    QFileInfo info(path);
    return info.exists() && info.isDir() && !info.isSymLink();
}
}

bool ParseUpdatePackage(const QByteArray &payload, const QString &version,
                        QVector<UpdateFile> *files, QString *error)
{
    if (!files || payload.size() > maxPackage || payload.isEmpty())
        return fail(error, QStringLiteral("Update package has an invalid size."));
    QJsonParseError parseError;
    const auto doc = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return fail(error, QStringLiteral("Update package is not valid JSON."));
    const auto root = doc.object();
    if (root.value("format").toInt() != 1 || root.value("version").toString() != version ||
        root.value("obs_major").toInt() != 33 || !root.value("files").isArray())
        return fail(error, QStringLiteral("Update package metadata does not match."));
    const auto entries = root.value("files").toArray();
    if (entries.isEmpty() || entries.size() > 128)
        return fail(error, QStringLiteral("Update package has an invalid file count."));
    QVector<UpdateFile> parsed;
    QSet<QString> names;
    bool hasDll = false;
    for (const auto &item : entries) {
        if (!item.isObject()) return fail(error, QStringLiteral("Invalid update file entry."));
        const auto object = item.toObject();
        const auto path = object.value("path").toString();
        const auto key = path.toCaseFolded();
        if ((path != "multistream-obs.dll" && path != tlsPath && !localePath.match(path).hasMatch()) || names.contains(key))
            return fail(error, QStringLiteral("Unsafe or duplicate update path."));
        if (!object.value("data").isString()) return fail(error, QStringLiteral("Missing update file data."));
        auto decoded = QByteArray::fromBase64Encoding(object.value("data").toString().toLatin1(),
                                                       QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded || decoded.decoded.isEmpty() || decoded.decoded.size() > maxFile)
            return fail(error, QStringLiteral("Invalid update file data."));
        if (path == "multistream-obs.dll" || path == tlsPath) {
            if (!decoded.decoded.startsWith("MZ")) return fail(error, QStringLiteral("Invalid plugin DLL."));
            if (path == "multistream-obs.dll") hasDll = true;
        }
        names.insert(key);
        parsed.push_back({path, std::move(decoded.decoded)});
    }
    if (!hasDll) return fail(error, QStringLiteral("Update package is missing the plugin DLL."));
    // Replacing the currently loaded DLL is the last operation in the transaction.
    std::stable_sort(parsed.begin(), parsed.end(), [](const UpdateFile &a, const UpdateFile &b) {
        return a.path != "multistream-obs.dll" && b.path == "multistream-obs.dll";
    });
    *files = std::move(parsed);
    return true;
}

bool ApplyUpdatePackage(const QString &directory, const QVector<UpdateFile> &files, QString *error)
{
    QDir base(directory);
    if (!plainDir(base.absolutePath()) || !plainDir(base.filePath("data")) ||
        !plainDir(base.filePath("data/locale")) || !plainDir(base.filePath("qt-plugins")) ||
        !plainDir(base.filePath("qt-plugins/tls")))
        return fail(error, QStringLiteral("Plugin directory is missing or is a symbolic link."));
    QLockFile lock(base.filePath(".multistream-update.lock"));
    lock.setStaleLockTime(30000);
    if (!lock.tryLock(0)) return fail(error, QStringLiteral("Another OBS instance is updating the plugin."));

    // Validate all destinations and pre-stage every file before changing any installed file.
    struct Entry { QString dest, staged, backup; bool existed = false, installed = false; };
    QVector<Entry> entries;
    for (const auto &file : files) {
        if (file.path != "multistream-obs.dll" && file.path != tlsPath && !localePath.match(file.path).hasMatch())
            return fail(error, QStringLiteral("Unsafe update path."));
        Entry e{base.filePath(file.path), base.filePath(file.path + ".mso-new"),
                base.filePath(file.path + ".mso-backup")};
        QFileInfo original(e.dest), staged(e.staged), backup(e.backup);
        if (original.isSymLink() || (original.exists() && !original.isFile()) ||
            staged.isSymLink() || backup.isSymLink())
            return fail(error, QStringLiteral("A plugin update path is not a normal file."));
        // A DLL backup can remain locked by another OBS instance; never overwrite it.
        if (backup.exists() && !original.exists()) {
            if (!QFile::rename(e.backup, e.dest))
                return fail(error, QStringLiteral("A previous update needs manual recovery from .mso-backup."));
            return fail(error, QStringLiteral("Recovered an interrupted update; restart OBS before trying again."));
        }
        if (backup.exists() && !QFile::remove(e.backup))
            return fail(error, QStringLiteral("A previous update is still in use. Close other OBS instances."));
        if (staged.exists() && !QFile::remove(e.staged))
            return fail(error, QStringLiteral("Cannot clear an old update staging file."));
        e.existed = original.exists();
        entries.push_back(e);
    }
    for (qsizetype i = 0; i < files.size(); ++i) {
        QSaveFile staged(entries[i].staged);
        if (!staged.open(QIODevice::WriteOnly) || staged.write(files[i].bytes) != files[i].bytes.size() || !staged.commit()) {
            for (const auto &e : entries) QFile::remove(e.staged);
            return fail(error, QStringLiteral("Cannot write the staged update. Check plugin folder permissions."));
        }
    }
    for (qsizetype i = 0; i < entries.size(); ++i) {
        auto &e = entries[i];
        const bool moved = !e.existed || QFile::rename(e.dest, e.backup);
        if (!moved || !QFile::rename(e.staged, e.dest)) {
            bool rollbackOk = true;
            if (moved && e.existed && !QFile::rename(e.backup, e.dest)) rollbackOk = false;
            for (qsizetype j = i; j-- > 0;) {
                auto &done = entries[j];
                if (!QFile::remove(done.dest)) rollbackOk = false;
                if (done.existed && !QFile::rename(done.backup, done.dest)) rollbackOk = false;
            }
            for (const auto &remaining : entries) QFile::remove(remaining.staged);
            return fail(error, rollbackOk ? QStringLiteral("Could not install update; original files restored.") :
                        QStringLiteral("Update failed and rollback was incomplete; restore the .mso-backup files."));
        }
        e.installed = true;
    }
    for (const auto &e : entries) if (e.existed) QFile::remove(e.backup);
    return true;
}
