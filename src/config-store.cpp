#include "config-store.h"

#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <json.hpp>
#include <unordered_set>
#include <utility>

bool IsValidMultistreamConfig(const QByteArray &content)
{
    if (content.isEmpty() || content.size() > MaxConfigBytes)
        return false;
    try {
        const auto json = nlohmann::json::parse(content.constData(), content.constData() + content.size());
        if (!json.is_object() || !json.contains("targets") || !json["targets"].is_array())
            return false;
        for (const char *field : {"targets", "video_configs", "audio_configs"}) {
            const auto it = json.find(field);
            if (it == json.end())
                continue; // Older exports may omit encoder arrays.
            if (!it->is_array())
                return false;
            std::unordered_set<std::string> ids;
            for (const auto &entry : *it) {
                if (!entry.is_object())
                    return false;
                const auto id = entry.find("id");
                if (id == entry.end() || !id->is_string())
                    return false;
                const auto value = id->get<std::string>();
                if (value.empty() || !ids.insert(value).second)
                    return false;
            }
        }
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

namespace {
enum class ReadState { Missing, Valid, Invalid };
struct ReadResult { ReadState state; QByteArray content; };

ReadResult ReadConfig(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink())
        return {ReadState::Missing, {}};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > MaxConfigBytes)
        return {ReadState::Invalid, {}};
    const auto content = file.read(MaxConfigBytes + 1);
    if (file.error() != QFileDevice::NoError || !IsValidMultistreamConfig(content))
        return {ReadState::Invalid, {}};
    return {ReadState::Valid, content};
}
}

bool WriteConfigAtomically(const QString &path, const QByteArray &content)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    if (file.write(content) != content.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

ConfigStore::ConfigStore(QString path) : path_(std::move(path)) {}

ConfigLoadResult ConfigStore::Load()
{
    const auto primary = ReadConfig(path_);
    if (primary.state == ReadState::Valid) {
        state_ = ConfigLoadState::Loaded;
        return {state_, primary.content};
    }
    const auto backup = ReadConfig(path_ + ".bak");
    if (backup.state == ReadState::Valid) {
        state_ = ConfigLoadState::Recovered;
        return {state_, backup.content};
    }
    state_ = primary.state == ReadState::Missing && backup.state == ReadState::Missing
                 ? ConfigLoadState::NewProfile : ConfigLoadState::Invalid;
    return {state_, {}};
}

bool ConfigStore::Save(const QByteArray &content, bool allowReplacement)
{
    if (!IsValidMultistreamConfig(content) || (state_ == ConfigLoadState::Invalid && !allowReplacement))
        return false;

    // Keep the last known-good backup when recovering a damaged primary file.
    if (state_ == ConfigLoadState::Loaded || state_ == ConfigLoadState::NewProfile) {
        const auto primary = ReadConfig(path_);
        if (primary.state == ReadState::Invalid) {
            state_ = ConfigLoadState::Invalid;
            if (!allowReplacement)
                return false;
        } else if (primary.state == ReadState::Valid && !WriteConfigAtomically(path_ + ".bak", primary.content)) {
            return false;
        }
    }
    if (!WriteConfigAtomically(path_, content))
        return false;
    state_ = ConfigLoadState::Loaded;
    return true;
}
