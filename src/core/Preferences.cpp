#include "core/Preferences.h"

#include <QFileInfo>

#include <unistd.h>

#include <algorithm>

namespace fr {
namespace {

constexpr int kMaxRecentFiles = 10;
constexpr int kMinCacheMegabytes = 64;
constexpr int kMaxCacheMegabytes = 32768;

// An eighth of the memory per viewer (two viewers: a quarter), between 512 MB and 6 GB. 1080p
// frames are about 8 MB, so 4 GB holds some 500 of them: a long loop plays from memory.
int defaultCacheMegabytes() {
#ifdef _SC_PHYS_PAGES
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long pageSize = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && pageSize > 0) {
        const qint64 megabytes = static_cast<qint64>(pages) * pageSize / (1024 * 1024);
        return static_cast<int>(std::clamp<qint64>(megabytes / 8, 512, 6144));
    }
#endif
    return 1024;
}

}  // namespace

Preferences::Preferences(const QString& iniPath) {
    if (iniPath.isEmpty()) {
        settings_ = std::make_unique<QSettings>(QStringLiteral("KenaePRO"), QStringLiteral("KenaePRO"));
        // The app was called framereview before; bring its settings along once.
        if (settings_->allKeys().isEmpty()) {
            const QSettings old(QStringLiteral("framereview"), QStringLiteral("framereview"));
            for (const QString& key : old.allKeys()) settings_->setValue(key, old.value(key));
        }
    } else {
        settings_ = std::make_unique<QSettings>(iniPath, QSettings::IniFormat);
    }
}

QString Preferences::ffmpegPath() const {
    return settings_->value(QStringLiteral("ffmpegPath")).toString();
}

void Preferences::setFfmpegPath(const QString& path) {
    settings_->setValue(QStringLiteral("ffmpegPath"), path);
}

int Preferences::cacheMegabytes() const {
    const int stored = settings_->value(QStringLiteral("cacheMegabytes"), 0).toInt();
    // 256 was the old default, saved by any OK in the dialog: it means "never chosen".
    if (stored <= 0 || stored == 256) return defaultCacheMegabytes();
    return std::clamp(stored, kMinCacheMegabytes, kMaxCacheMegabytes);
}

void Preferences::setCacheMegabytes(int megabytes) {
    settings_->setValue(QStringLiteral("cacheMegabytes"), std::clamp(megabytes, kMinCacheMegabytes, kMaxCacheMegabytes));
}

int Preferences::audioBufferMs() const {
    return std::clamp(settings_->value(QStringLiteral("audioBufferMs"), 100).toInt(), 20, 1000);
}

void Preferences::setAudioBufferMs(int milliseconds) {
    settings_->setValue(QStringLiteral("audioBufferMs"), std::clamp(milliseconds, 20, 1000));
}

double Preferences::sequenceFrameRate() const {
    const double fps = settings_->value(QStringLiteral("sequenceFrameRate"), 24.0).toDouble();
    return fps > 0.0 ? fps : 24.0;
}

void Preferences::setSequenceFrameRate(double fps) {
    settings_->setValue(QStringLiteral("sequenceFrameRate"), fps > 0.0 ? fps : 24.0);
}

bool Preferences::confirmOnClose() const {
    return settings_->value(QStringLiteral("confirmOnClose"), true).toBool();
}

void Preferences::setConfirmOnClose(bool confirm) {
    settings_->setValue(QStringLiteral("confirmOnClose"), confirm);
}

bool Preferences::apiEnabled() const {
    return settings_->value(QStringLiteral("apiEnabled"), false).toBool();
}

quint16 Preferences::apiPort() const {
    const int port = settings_->value(QStringLiteral("apiPort"), 4545).toInt();
    return static_cast<quint16>(std::clamp(port, 1, 65535));
}

void Preferences::setApi(bool enabled, quint16 port) {
    settings_->setValue(QStringLiteral("apiEnabled"), enabled);
    settings_->setValue(QStringLiteral("apiPort"), static_cast<int>(port));
}

bool Preferences::audioMuted() const {
    return settings_->value(QStringLiteral("audioMuted"), false).toBool();
}

void Preferences::setAudioMuted(bool muted) {
    settings_->setValue(QStringLiteral("audioMuted"), muted);
}

double Preferences::audioVolume() const {
    return std::clamp(settings_->value(QStringLiteral("audioVolume"), 1.0).toDouble(), 0.0, 1.0);
}

void Preferences::setAudioVolume(double volume) {
    settings_->setValue(QStringLiteral("audioVolume"), std::clamp(volume, 0.0, 1.0));
}

bool Preferences::audioScrub() const {
    return settings_->value(QStringLiteral("audioScrub"), true).toBool();
}

void Preferences::setAudioScrub(bool scrub) {
    settings_->setValue(QStringLiteral("audioScrub"), scrub);
}

int Preferences::numberingMode() const {
    return std::clamp(settings_->value(QStringLiteral("numberingMode"), 0).toInt(), 0, 3);
}

void Preferences::setNumberingMode(int mode) {
    settings_->setValue(QStringLiteral("numberingMode"), std::clamp(mode, 0, 3));
}

double Preferences::scrubPixelsPerFrame() const {
    return std::clamp(settings_->value(QStringLiteral("scrubPixelsPerFrame"), 8.0).toDouble(), 1.0, 50.0);
}

void Preferences::setScrubPixelsPerFrame(double pixels) {
    settings_->setValue(QStringLiteral("scrubPixelsPerFrame"), std::clamp(pixels, 1.0, 50.0));
}

bool Preferences::alwaysOnTop() const {
    return settings_->value(QStringLiteral("alwaysOnTop"), false).toBool();
}

void Preferences::setAlwaysOnTop(bool onTop) {
    settings_->setValue(QStringLiteral("alwaysOnTop"), onTop);
}

QByteArray Preferences::windowGeometry() const {
    return settings_->value(QStringLiteral("window/geometry")).toByteArray();
}

QByteArray Preferences::windowState() const {
    return settings_->value(QStringLiteral("window/state")).toByteArray();
}

void Preferences::setWindowLayout(const QByteArray& geometry, const QByteArray& state) {
    settings_->setValue(QStringLiteral("window/geometry"), geometry);
    settings_->setValue(QStringLiteral("window/state"), state);
}

void Preferences::clearWindowLayout() {
    settings_->remove(QStringLiteral("window"));
}

QHash<QString, QString> Preferences::shortcuts() const {
    QHash<QString, QString> result;
    settings_->beginGroup(QStringLiteral("shortcuts"));
    for (const QString& key : settings_->childKeys()) result.insert(key, settings_->value(key).toString());
    settings_->endGroup();
    return result;
}

void Preferences::setShortcuts(const QHash<QString, QString>& shortcuts) {
    settings_->remove(QStringLiteral("shortcuts"));
    settings_->beginGroup(QStringLiteral("shortcuts"));
    for (auto it = shortcuts.cbegin(); it != shortcuts.cend(); ++it) settings_->setValue(it.key(), it.value());
    settings_->endGroup();
}

QStringList Preferences::recentFiles() const {
    return settings_->value(QStringLiteral("recentFiles")).toStringList();
}

void Preferences::addRecentFile(const QString& path) {
    const QString absolute = QFileInfo(path).absoluteFilePath();
    QStringList files = recentFiles();
    files.removeAll(absolute);
    files.prepend(absolute);
    while (files.size() > kMaxRecentFiles) files.removeLast();
    settings_->setValue(QStringLiteral("recentFiles"), files);
}

void Preferences::clearRecentFiles() {
    settings_->remove(QStringLiteral("recentFiles"));
}

}  // namespace fr
