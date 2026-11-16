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
    if (!failures) std::cout << "All platform helper tests passed.\n";
    return failures ? 1 : 0;
}
