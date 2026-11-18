#include "config-store.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <iostream>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    int failures = 0;
    auto check = [&](bool ok, const char *name) {
        if (!ok) { std::cerr << "FAIL: " << name << '\n'; ++failures; }
    };
    const QByteArray empty = R"({"targets":[],"video_configs":[],"audio_configs":[]})";
    const QByteArray original = R"({"targets":[{"id":"one","name":"Test"}],"video_configs":[],"audio_configs":[]})";
    const QByteArray updated = R"({"targets":[{"id":"two"}]})";
    check(IsValidMultistreamConfig(empty), "accept an intentional empty configuration");
    check(IsValidMultistreamConfig(updated), "accept legacy exports without encoder lists");
    check(IsValidMultistreamConfig(QByteArray("\xef\xbb\xbf") + original), "accept OBS UTF-8 BOM");
    check(!IsValidMultistreamConfig("{}"), "reject unrelated object");
    check(!IsValidMultistreamConfig("[]"), "reject array root");
    check(!IsValidMultistreamConfig("{\"targets\":["), "reject truncated JSON");
    check(!IsValidMultistreamConfig(R"({"targets":{}})"), "reject invalid targets list");
    check(!IsValidMultistreamConfig(R"({"targets":[{}]})"), "reject target without ID");
    check(!IsValidMultistreamConfig(R"({"targets":[{"id":"x"},{"id":"x"}]})"), "reject duplicate target IDs");
    check(!IsValidMultistreamConfig(R"({"targets":[],"video_configs":{}})"), "reject invalid encoder list");
    check(!IsValidMultistreamConfig(QByteArray(MaxConfigBytes + 1, 'x')), "reject oversized configuration");

    QTemporaryDir temp;
    check(temp.isValid(), "create temporary test directory");
    const QString path = temp.filePath("settings.json");
    auto write = [&](const QString &where, const QByteArray &bytes) {
        QFile f(where);
        return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size();
    };
    auto read = [&](const QString &where) {
        QFile f(where);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    };

    ConfigStore store(path);
    check(!store.Save(original), "do not save before loading");
    check(store.Load().state == ConfigLoadState::NewProfile, "new profile starts empty");
    check(store.Save(original), "save new profile");
    check(store.Save(updated), "save changed profile");
    check(read(path) == updated && read(path + ".bak") == original, "backup contains previous valid configuration");

    check(write(path, "broken"), "simulate corrupt primary");
    ConfigStore recovered(path);
    const auto loaded = recovered.Load();
    check(loaded.state == ConfigLoadState::Recovered && loaded.content == original, "recover valid backup");
    check(recovered.Save(updated), "save after recovery");
    check(read(path) == updated && read(path + ".bak") == original, "recovery preserves good backup");

    check(write(path, "broken") && write(path + ".bak", "also broken"), "simulate two damaged files");
    ConfigStore blocked(path);
    check(blocked.Load().state == ConfigLoadState::Invalid, "detect two damaged files");
    check(!blocked.Save(empty), "block automatic overwrite");
    check(read(path) == "broken" && read(path + ".bak") == "also broken", "blocked save preserves both files");
    check(blocked.Save(original, true), "allow explicitly confirmed restore");
    check(read(path) == original && read(path + ".bak") == "also broken", "restore leaves old backup untouched");

    check(write(path, "external damage"), "simulate external damage after load");
    check(!blocked.Save(empty), "detect external corruption before rotating backup");
    check(read(path) == "external damage" && read(path + ".bak") == "also broken", "external damage does not destroy backup");

    const QString blockedPath = temp.filePath("blocked.json");
    check(write(blockedPath, original) && QDir().mkdir(blockedPath + ".bak"), "create unwritable backup destination");
    ConfigStore diskFailure(blockedPath);
    check(diskFailure.Load().state == ConfigLoadState::Loaded, "load before simulated write failure");
    check(!diskFailure.Save(updated), "report failed backup write");
    check(read(blockedPath) == original, "failed backup write leaves primary untouched");

    if (!failures) std::cout << "All configuration safety tests passed.\n";
    return failures ? 1 : 0;
}
