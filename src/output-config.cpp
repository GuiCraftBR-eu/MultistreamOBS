// Modified for Multistream OBS on 2026-11-16.
#include "output-config.h"
#include "pch.h"

#include <obs.h>
#include <obs-frontend-api.h>
#include <random>
#include <filesystem>
#include <unordered_set>
#include <algorithm>
#include <util/platform.h>
#include "json-util.hpp"
#include "config-store.h"


MultiOutputConfig& GlobalMultiOutputConfig()
{
    static MultiOutputConfig instance;
    return instance;
}


static nlohmann::json SaveTarget(OutputTargetConfig& config) {
    nlohmann::json json;
    json["id"] = config.id;
    json["name"] = config.name;
    if (!config.platform.empty())
        json["platform"] = config.platform;
    json["protocol"] = config.protocol;
    json["service-param"] = config.serviceParam;
    json["output-param"] = config.outputParam;
    json["sync-start"] = config.syncStart;
    json["sync-stop"] = config.syncStop;
    json["enabled"] = config.enabled;
    if (config.videoConfig.has_value())
        json["video-config"] = *config.videoConfig;
    if (config.audioConfig.has_value())
        json["audio-config"] = *config.audioConfig;
    return json;
}

static nlohmann::json SaveVideoConfig(VideoEncoderConfig& config) {
    nlohmann::json json;
    json["id"] = config.id;
    json["encoder"] = config.encoderId;
    json["param"] = config.encoderParams;
    if (config.outputScene.has_value())
        json["scene"] = *config.outputScene;
    if (config.resolution.has_value())
        json["resolution"] = *config.resolution;
    json["fps-denumerator"] = config.fpsDenumerator;
    return json;
}

static nlohmann::json SaveAudioTrackConfig(AudioTrackConfig &config) {
	nlohmann::json json;
	json["mixer_track"] = config.mixer_track;
	json["output_track"] = config.output_track;
	return json;
}

static nlohmann::json SaveAudioConfig(AudioEncoderConfig& config) {
    nlohmann::json json;
    json["id"] = config.id;
    json["encoder"] = config.encoderId;
    json["param"] = config.encoderParams;
    json["mixerId"] = config.mixerId;


    nlohmann::json audio_tracks(nlohmann::json::value_t::array);
    for(auto& track: config.audioTracks) {
        audio_tracks.push_back(SaveAudioTrackConfig(*track));
    }

    json["audioTracks"] = audio_tracks;

    return json;
}

static std::string SaveMultiOutputConfig(MultiOutputConfig& config) {
    nlohmann::json json;

    std::unordered_set<std::string> videoconfig_in_use;
    std::unordered_set<std::string> audioconfig_in_use;

    int target_count = 0, videocfg_count = 0, audiocfg_count = 0;

    nlohmann::json targets(nlohmann::json::value_t::array);
    for(auto& target: config.targets) {
        targets.push_back(SaveTarget(*target));
        if (target->videoConfig.has_value())
            videoconfig_in_use.insert(*target->videoConfig);
        if (target->audioConfig.has_value())
            audioconfig_in_use.insert(*target->audioConfig);
        ++target_count;
    }

    nlohmann::json video_configs(nlohmann::json::value_t::array);
    for(auto& video_config: config.videoConfig) {
        if (videoconfig_in_use.find(video_config->id) != videoconfig_in_use.end())
            video_configs.push_back(SaveVideoConfig(*video_config));
        ++videocfg_count;
    }

    nlohmann::json audio_configs(nlohmann::json::value_t::array);
    for(auto& audio_config: config.audioConfig) {
        if (audioconfig_in_use.find(audio_config->id) != audioconfig_in_use.end())
            audio_configs.push_back(SaveAudioConfig(*audio_config));
        ++audiocfg_count;
    }

    json["targets"] = targets;
    json["video_configs"] = video_configs;
    json["audio_configs"] = audio_configs;

    blog(LOG_INFO, TAG "Save %d targets, %d video configs, %d audio configs", target_count, videocfg_count, audiocfg_count);

    return json.dump();
}




static OutputTargetConfigPtr LoadTargetConfig(nlohmann::json& json) {
    auto id = GetJsonField<std::string>(json, "id");
    if (!id.has_value())
        return {};

    auto config = std::make_shared<OutputTargetConfig>();
    config->id = *id;
    config->name = GetJsonField<std::string>(json, "name").value_or("");
    config->platform = GetJsonField<std::string>(json, "platform").value_or("");
    config->protocol = GetJsonField<std::string>(json, "protocol").value_or("RTMP"); // for compatibility
    config->syncStart = GetJsonField<bool>(json, "sync-start").value_or(false);
    config->syncStop = GetJsonField<bool>(json, "sync-stop").value_or(config->syncStart);
    config->enabled = GetJsonField<bool>(json, "enabled").value_or(true);
    config->serviceParam = GetJsonField<nlohmann::json>(json, "service-param").value_or(nlohmann::json{});
    config->outputParam = GetJsonField<nlohmann::json>(json, "output-param").value_or(nlohmann::json{});
    config->videoConfig = GetJsonField<std::string>(json, "video-config");
    config->audioConfig = GetJsonField<std::string>(json, "audio-config");

    return config;
}

static VideoEncoderConfigPtr LoadVideoConfig(nlohmann::json& json) {
    auto id = GetJsonField<std::string>(json, "id");
    if (!id.has_value())
        return {};

    auto config = std::make_shared<VideoEncoderConfig>();
    config->id = *id;
    config->encoderId = GetJsonField<std::string>(json, "encoder").value_or("");
    config->outputScene = GetJsonField<std::string>(json, "scene");
    config->resolution = GetJsonField<std::string>(json, "resolution");
    config->fpsDenumerator = GetJsonField<int>(json, "fps-denumerator").value_or(1);
    config->encoderParams = GetJsonField<nlohmann::json>(json, "param").value_or(nlohmann::json{});

    return config;
}

static AudioTrackConfigPtr LoadAudioTrackConfig(nlohmann::json& json) {
    auto config = std::make_shared<AudioTrackConfig>();
    config->mixer_track = GetJsonField<int>(json, "mixer_track").value_or(0);
    config->output_track = GetJsonField<int>(json, "output_track").value_or(0);

    return config;
}

static AudioEncoderConfigPtr LoadAudioConfig(nlohmann::json& json) {
    auto id = GetJsonField<std::string>(json, "id");
    if (!id.has_value())
        return {};
    
    auto config = std::make_shared<AudioEncoderConfig>();
    config->id = *id;
    config->encoderId = GetJsonField<std::string>(json, "encoder").value_or("");
    config->mixerId = GetJsonField<int>(json, "mixerId").value_or(0);
    config->encoderParams = GetJsonField<nlohmann::json>(json, "param").value_or(nlohmann::json{});

    auto it = json.find("audioTracks");
    if (it != json.end() && it->type() == nlohmann::json::value_t::array) {
        for(auto& audio_track_json: *it) {
            if (audio_track_json.type() != nlohmann::json::value_t::object)
                continue;
            auto audio_track = LoadAudioTrackConfig(audio_track_json);
            if (audio_track)
                config->audioTracks.emplace_back(audio_track);
        }
    }

    return config;
}

static std::optional<MultiOutputConfig> ParseMultiOutputConfig(const std::string& content) {
    try {
        int target_count = 0, videocfg_count = 0, audiocfg_count = 0;

        auto json = nlohmann::json::parse(content);
        MultiOutputConfig config;
        auto it = json.find("targets");
        if (it != json.end() && it->type() == nlohmann::json::value_t::array) {
            for(auto& target_json: *it) {
                if (target_json.type() != nlohmann::json::value_t::object)
                    continue;
                auto target = LoadTargetConfig(target_json);
                if (target)
                    config.targets.emplace_back(target);
                ++target_count;
            }
        }

        it = json.find("video_configs");
        if (it != json.end() && it->type() == nlohmann::json::value_t::array) {
            for(auto& video_enc_json: *it) {
                if (video_enc_json.type() != nlohmann::json::value_t::object)
                    continue;
                auto video_enc = LoadVideoConfig(video_enc_json);
                if (video_enc) {
                    config.videoConfig.emplace_back(video_enc);
                }
                ++videocfg_count;
            }
        }

        it = json.find("audio_configs");
        if (it != json.end() && it->type() == nlohmann::json::value_t::array) {
            for(auto& audio_enc_json: *it) {
                if (audio_enc_json.type() != nlohmann::json::value_t::object)
                    continue;
                auto audio_enc = LoadAudioConfig(audio_enc_json);
                if (audio_enc) {
                    config.audioConfig.emplace_back(audio_enc);
                }
                ++audiocfg_count;
            }
        }

        blog(LOG_INFO, TAG "Load %d targets, %d video configs, %d audio configs", target_count, videocfg_count, audiocfg_count);
        
        return config;
    }
    catch(const std::exception& e) {
        (void)e; // Parser details may contain private configuration data.
        blog(LOG_ERROR, TAG "Failed to parse configuration.");
        return std::nullopt;
    }
}

std::string SerializeMultiOutputConfig(MultiOutputConfig& config) {
    return SaveMultiOutputConfig(config);
}

std::optional<MultiOutputConfig> DeserializeMultiOutputConfig(const std::string& content) {
    if (content.size() > static_cast<size_t>(MaxConfigBytes) ||
        !IsValidMultistreamConfig(QByteArray(content.data(), static_cast<qsizetype>(content.size()))))
        return std::nullopt;
    return ParseMultiOutputConfig(content);
}

namespace {
std::unique_ptr<ConfigStore> configStore;
QString configPath;
bool parseFailed = false;
bool recoveryRequired = false;
std::function<void(ConfigStatus)> statusCallback;

QString CurrentConfigPath() {
    auto dir = obs_frontend_get_current_profile_path();
    if (!dir)
        return {};
    const QString path = QString::fromUtf8(dir) + "/multistream-obs.json";
    bfree(dir);
    return path;
}

void ReportStatus(ConfigStatus status) {
    if (statusCallback)
        statusCallback(status);
}
}

void SetConfigStatusCallback(std::function<void(ConfigStatus)> callback) {
    statusCallback = std::move(callback);
}

bool SaveMultiOutputConfig(MultiOutputConfig *replacement, bool allowReplacement) {
    if (!configStore || configPath.isEmpty() || CurrentConfigPath() != configPath ||
        ((parseFailed || recoveryRequired) && !allowReplacement)) {
        blog(LOG_ERROR, TAG "Configuration save blocked to protect existing files.");
        ReportStatus(ConfigStatus::ReadError);
        return false;
    }
    auto &config = replacement ? *replacement : GlobalMultiOutputConfig();
    const auto content = SaveMultiOutputConfig(config);
    if (!configStore->Save(QByteArray(content.data(), static_cast<qsizetype>(content.size())), allowReplacement)) {
        blog(LOG_ERROR, TAG "Failed to save configuration; existing files were preserved.");
        ReportStatus(recoveryRequired ? ConfigStatus::ReadError : ConfigStatus::WriteError);
        return false;
    }
    parseFailed = false;
    recoveryRequired = false;
    ReportStatus(ConfigStatus::Ready);
    return true;
}

bool LoadMultiOutputConfig() {
    configPath = CurrentConfigPath();
    parseFailed = false;
    recoveryRequired = false;
    if (configPath.isEmpty()) {
        configStore.reset();
        ReportStatus(ConfigStatus::ReadError);
        return false;
    }
    configStore = std::make_unique<ConfigStore>(configPath);
    const auto result = configStore->Load();
    if (result.state == ConfigLoadState::NewProfile) {
        ReportStatus(ConfigStatus::Ready);
        return false;
    }
    if (result.state == ConfigLoadState::Invalid) {
        recoveryRequired = true;
        blog(LOG_ERROR, TAG "Configuration and backup are unreadable; automatic saving is disabled.");
        ReportStatus(ConfigStatus::ReadError);
        return false;
    }
    const auto parsed = DeserializeMultiOutputConfig(std::string(result.content.constData(), static_cast<size_t>(result.content.size())));
    if (!parsed) {
        parseFailed = true;
        recoveryRequired = true;
        ReportStatus(ConfigStatus::ReadError);
        return false;
    }
    GlobalMultiOutputConfig() = *parsed;
    if (result.state == ConfigLoadState::Recovered) {
        blog(LOG_WARNING, TAG "Recovered configuration from backup.");
        ReportStatus(ConfigStatus::Recovered);
    } else {
        ReportStatus(ConfigStatus::Ready);
    }
    return true;
}


template<class T>
static bool has_id(T& container, const std::string& id) {
    for(auto& item: container) {
        if (item->id == id)
            return true;
    }
    return false;
}

std::string GenerateId(MultiOutputConfig& config) {
    static std::random_device rndgen;
    for(;;) {
        auto rndnum = rndgen();
        auto newid = std::to_string(rndnum);
        if (has_id(config.targets, newid))
            continue;
        if (has_id(config.audioConfig, newid))
            continue;
        if (has_id(config.videoConfig, newid))
            continue;
        return newid;
    }
}
