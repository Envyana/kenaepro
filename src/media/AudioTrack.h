#pragma once

#include <QByteArray>
#include <QString>

#include <atomic>
#include <future>
#include <mutex>
#include <thread>

namespace fr {

// The sound of a media file, decoded to 48 kHz stereo 16-bit PCM on a background thread. Playback
// can start while decoding is still running; what is not decoded yet reads as silence. Sound
// beyond kMaxBytes (about 45 minutes) is dropped.
class AudioTrack {
public:
    static constexpr int kSampleRate = 48000;
    static constexpr int kChannels = 2;
    static constexpr int kBytesPerFrame = kChannels * 2;  // one audio frame = one sample per channel
    static constexpr qint64 kMaxBytes = 512LL * 1024 * 1024;

    AudioTrack() = default;
    ~AudioTrack();
    AudioTrack(const AudioTrack&) = delete;
    AudioTrack& operator=(const AudioTrack&) = delete;

    // Starts decoding. False (with a reason) when the file has no sound or cannot be opened, or
    // when this build has no FFmpeg.
    bool open(const QString& path, QString* error = nullptr);
    void close();  // stops the decoder and drops the data

    bool hasAudio() const { return opened_; }
    bool isFinished() const { return finished_; }
    qint64 decodedFrames() const;
    double decodedSeconds() const { return static_cast<double>(decodedFrames()) / kSampleRate; }

    // Exactly `frames` audio frames from `startFrame`, zero-filled before the start of the track and
    // where nothing is decoded yet.
    QByteArray read(qint64 startFrame, qint64 frames) const;

private:
    void decode(const QString& path, std::promise<QString>& probe);

    bool opened_ = false;
    std::atomic<bool> finished_{false};
    std::atomic<bool> stop_{false};
    mutable std::mutex mutex_;
    QByteArray pcm_;
    std::thread thread_;
};

}  // namespace fr
