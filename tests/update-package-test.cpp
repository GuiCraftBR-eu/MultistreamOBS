#include "update-package.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibrary>
#include <QTemporaryDir>
#include <iostream>
#ifdef _WIN32
#include <Windows.h>
#endif

static bool expect(bool condition, const char *message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}
static QByteArray payload(const QJsonArray &files, QString version = "1.1.0") {
    return QJsonDocument(QJsonObject{{"format", 1}, {"version", version}, {"obs_major", 33}, {"files", files}})
        .toJson(QJsonDocument::Compact);
}
static QJsonObject item(const QString &path, const QByteArray &bytes) {
    return {{"path", path}, {"data", QString::fromLatin1(bytes.toBase64())}};
}
static bool write(const QString &path, const QByteArray &data) {
    QFile f(path); return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}
static QByteArray read(const QString &path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray{}; }

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    bool ok = true;
    QVector<UpdateFile> files;
    QString error;
    const QJsonArray valid{item("multistream-obs.dll", "MZ-new"), item("data/locale/en-US.ini", "new locale")};
    ok &= expect(ParseUpdatePackage(payload(valid), "1.1.0", &files, &error), "parse valid package");
    ok &= expect(files.size() == 2 && files.back().path == "multistream-obs.dll", "install DLL last");
    QTemporaryDir temp;
    QDir base(temp.path());
    ok &= expect(temp.isValid() && base.mkpath("data/locale") && base.mkpath("qt-plugins/tls"), "create temporary plugin folder");
    ok &= expect(write(base.filePath("multistream-obs.dll"), "MZ-old"), "write old DLL");
    ok &= expect(write(base.filePath("data/locale/en-US.ini"), "old locale"), "write old locale");
    ok &= expect(ApplyUpdatePackage(temp.path(), files, &error), "apply valid package");
    ok &= expect(read(base.filePath("multistream-obs.dll")) == "MZ-new", "replace DLL");
    ok &= expect(read(base.filePath("data/locale/en-US.ini")) == "new locale", "replace locale");
    ok &= expect(!ParseUpdatePackage(payload(valid), "1.2.0", &files, &error), "reject wrong version");
    ok &= expect(!ParseUpdatePackage(payload(QJsonArray{item("../outside.dll", "MZ")}), "1.1.0", &files, &error), "reject path traversal");
    ok &= expect(!ParseUpdatePackage(payload(QJsonArray{item("multistream-obs.dll", "MZ"), item("data/locale/en-US.ini", "x"), item("data/locale/EN-us.ini", "y")}), "1.1.0", &files, &error), "reject case-insensitive duplicate");
    ok &= expect(!ParseUpdatePackage(payload(QJsonArray{item("data/locale/en-US.ini", "x")}), "1.1.0", &files, &error), "reject missing DLL");
    ok &= expect(!ParseUpdatePackage(payload(QJsonArray{item("multistream-obs.dll", "not an executable")}), "1.1.0", &files, &error), "reject invalid DLL");
    QJsonArray invalidBase64{QJsonObject{{"path", "multistream-obs.dll"}, {"data", "not-base64!"}}};
    ok &= expect(!ParseUpdatePackage(payload(invalidBase64), "1.1.0", &files, &error), "reject invalid base64");
    if (argc > 1) {
        const QByteArray realPackage = read(QString::fromLocal8Bit(argv[1]));
        QVector<UpdateFile> realFiles;
        ok &= expect(ParseUpdatePackage(realPackage, "1.1.0", &realFiles, &error), "parse generated release package");
        ok &= expect(realFiles.size() > 2, "generated package includes locales");
    }
#ifdef _WIN32
    // Fail the last replacement and verify that earlier locale changes roll back.
    QTemporaryDir rollbackTemp;
    QDir rollback(rollbackTemp.path());
    rollback.mkpath("data/locale");
    rollback.mkpath("qt-plugins/tls");
    const auto lockedPath = rollback.filePath("multistream-obs.dll");
    write(lockedPath, "MZ-old");
    write(rollback.filePath("data/locale/en-US.ini"), "old locale");
    HANDLE lockHandle = CreateFileW(reinterpret_cast<LPCWSTR>(lockedPath.utf16()), GENERIC_READ, 0, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ok &= expect(lockHandle != INVALID_HANDLE_VALUE, "lock DLL to simulate replacement failure");
    QVector<UpdateFile> rollbackFiles{{"data/locale/en-US.ini", "new locale"}, {"multistream-obs.dll", "MZ-new"}};
    ok &= expect(!ApplyUpdatePackage(rollbackTemp.path(), rollbackFiles, &error), "detect locked DLL failure");
    if (lockHandle != INVALID_HANDLE_VALUE) CloseHandle(lockHandle);
    ok &= expect(read(lockedPath) == "MZ-old", "preserve locked DLL");
    ok &= expect(read(rollback.filePath("data/locale/en-US.ini")) == "old locale", "restore locale on failure");
    // Windows keeps loaded modules open. Exercise replacement while the old DLL is loaded.
    QTemporaryDir loadedTemp;
    QDir loaded(loadedTemp.path());
    loaded.mkpath("data/locale");
    loaded.mkpath("qt-plugins/tls");
    const auto systemDll = QDir(qEnvironmentVariable("SystemRoot")).filePath("System32/version.dll");
    ok &= expect(QFile::copy(systemDll, loaded.filePath("multistream-obs.dll")), "copy test DLL");
    QLibrary oldDll(loaded.filePath("multistream-obs.dll"));
    ok &= expect(oldDll.load(), "load old test DLL");
    QVector<UpdateFile> replacement{{"multistream-obs.dll", "MZ-new-version"}};
    ok &= expect(ApplyUpdatePackage(loadedTemp.path(), replacement, &error), "replace DLL while loaded");
    ok &= expect(read(loaded.filePath("multistream-obs.dll")) == "MZ-new-version", "loaded-DLL replacement has new bytes");
    oldDll.unload();
#endif
    if (ok) std::cout << "All update package tests passed.\n";
    return ok ? 0 : 1;
}
