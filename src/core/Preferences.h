#pragma once

#include <QByteArray>
#include <QHash>
#include <QSettings>
#include <QString>
#include <QStringList>

#include <memory>

namespace fr {

// Application settings that survive between runs, kept in a QSettings file. With an empty path
// they live in the platform's usual place; tests pass the path of an .ini file instead.
class Preferences {
public:
    explicit Preferences(const QString& iniPath = {});

    QString ffmpegPath() const;  // empty: look in PATH
    void setFfmpegPath(const QString& path);

    int cacheMegabytes() const;  // frame cache budget per viewer, 64..32768; default from the memory
    void setCacheMegabytes(int megabytes);

    int audioBufferMs() const;  // 20..1000
    void setAudioBufferMs(int milliseconds);

    double sequenceFrameRate() const;  // for image sequences, which have no rate of their own
    void setSequenceFrameRate(double fps);

    bool confirmOnClose() const;
    void setConfirmOnClose(bool confirm);

    bool apiEnabled() const;  // the local Client API (Blender sync, scripts)
    quint16 apiPort() const;
    void setApi(bool enabled, quint16 port);

    bool audioMuted() const;
    void setAudioMuted(bool muted);
    double audioVolume() const;  // 0..1
    void setAudioVolume(double volume);
    bool audioScrub() const;
    void setAudioScrub(bool scrub);

    int numberingMode() const;  // a NumberingMode as a number
    void setNumberingMode(int mode);

    // Screen pixels of right-button drag in the viewer per frame (Shift scrubs ten times faster).
    double scrubPixelsPerFrame() const;  // 1..50
    void setScrubPixelsPerFrame(double pixels);

    bool alwaysOnTop() const;
    void setAlwaysOnTop(bool onTop);

    // Window size and the arrangement of the panels, as QMainWindow saves them. Empty: never saved.
    QByteArray windowGeometry() const;
    QByteArray windowState() const;
    void setWindowLayout(const QByteArray& geometry, const QByteArray& state);
    void clearWindowLayout();

    // Keys the user changed: command id -> key sequence text (empty text: no key).
    QHash<QString, QString> shortcuts() const;
    void setShortcuts(const QHash<QString, QString>& shortcuts);

    QStringList recentFiles() const;
    void addRecentFile(const QString& path);  // newest first, at most 10
    void clearRecentFiles();

    void sync() { settings_->sync(); }

private:
    std::unique_ptr<QSettings> settings_;
};

}  // namespace fr
