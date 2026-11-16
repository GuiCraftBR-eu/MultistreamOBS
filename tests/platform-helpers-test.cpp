#include "platform-helpers.h"
#include <QCoreApplication>
#include <iostream>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    int failures = 0;
    auto check = [&](bool good, const char *name) {
        if (!good) { std::cerr << "FAIL: " << name << '\n'; ++failures; }
    };
    const QString root = "rtmps://example.global-contribute.live-video.net";
    check(IsKickTarget("kick", "Other"), "explicit Kick platform");
    check(IsKickTarget("", "Kick"), "legacy Kick target");
    check(!IsKickTarget("twitch", "Kick"), "explicit Twitch is not Kick");
    check(!IsKickTarget("", "Twitch"), "Twitch is not Kick");
    check(NormalizeKickServer(root) == root + ":443/app", "complete root URL");
    check(NormalizeKickServer(root + "/") == root + ":443/app", "complete slash URL");
    check(NormalizeKickServer(root + ":443/app") == root + ":443/app", "preserve complete URL");
    check(NormalizeKickServer(root + "/special") == root + "/special", "preserve explicit path");
    check(NormalizeKickServer(root + "?token=x") == root + "?token=x", "preserve query URL");
    check(NormalizeKickServer("rtmps://other.example") == "rtmps://other.example", "preserve other host");
    const auto options = ObsResolutionOptions(1920, 1080);
    check(options == QStringList({"1920x1080", "1536x864", "1440x810", "1280x720", "1152x648",
                                  "1096x616", "960x540", "852x480", "768x432", "698x392", "640x360"}),
          "OBS resolution list for a 1920x1080 canvas");
    check(ObsResolutionOptions(0, 0).isEmpty(), "no zero-sized resolution");
    QJsonObject ingestion{{"rtmpsIngestionAddress", "rtmps://a.rtmps.youtube.com:443/live2"},
                          {"streamName", "test-key"}};
    QJsonObject item{{"snippet", QJsonObject{{"title", "Test stream"}, {"isDefaultStream", true}}},
                     {"cdn", QJsonObject{{"ingestionType", "rtmp"}, {"ingestionInfo", ingestion}}}};
    YouTubeStreamSettings youtube;
    check(YouTubeStreamFromApi(item, &youtube) && youtube.title == "Test stream" &&
          youtube.server == "rtmps://a.rtmps.youtube.com:443/live2" && youtube.key == "test-key" && youtube.isDefault,
          "parse YouTube RTMPS stream without changing the key");
    ingestion["rtmpsIngestionAddress"] = "rtmps://evil.example/live2";
    item["cdn"] = QJsonObject{{"ingestionType", "rtmp"}, {"ingestionInfo", ingestion}};
    check(!YouTubeStreamFromApi(item, &youtube), "reject untrusted YouTube server");
    ingestion["rtmpsIngestionAddress"] = "rtmps://a.rtmps.youtube.com/live2/test-key";
    item["cdn"] = QJsonObject{{"ingestionType", "rtmp"}, {"ingestionInfo", ingestion}};
    check(!YouTubeStreamFromApi(item, &youtube), "reject YouTube server containing the stream key");
    ingestion["rtmpsIngestionAddress"] = "rtmp://a.rtmps.youtube.com/live2";
    item["cdn"] = QJsonObject{{"ingestionType", "rtmp"}, {"ingestionInfo", ingestion}};
    check(!YouTubeStreamFromApi(item, &youtube), "reject unencrypted YouTube server");
    if (!failures) std::cout << "All platform helper tests passed.\n";
    return failures ? 1 : 0;
}

