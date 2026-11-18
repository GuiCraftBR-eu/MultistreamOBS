#pragma once

#include <QByteArray>
#include <QString>

// Profile files and imports share the same validation and size limit.
constexpr qint64 MaxConfigBytes = 16 * 1024 * 1024;

enum class ConfigLoadState { NewProfile, Loaded, Recovered, Invalid };

struct ConfigLoadResult {
    ConfigLoadState state;
    QByteArray content;
};

bool IsValidMultistreamConfig(const QByteArray &content);
bool WriteConfigAtomically(const QString &path, const QByteArray &content);

class ConfigStore {
public:
    explicit ConfigStore(QString path);
    ConfigLoadResult Load();
    // Explicit imports may replace an unreadable file after user confirmation.
    bool Save(const QByteArray &content, bool allowReplacement = false);

private:
    QString path_;
    ConfigLoadState state_ = ConfigLoadState::Invalid;
};
