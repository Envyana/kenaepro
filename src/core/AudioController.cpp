#include "core/AudioController.h"

#include "core/Session.h"
#include "media/AudioMix.h"
#include "media/AudioTrack.h"
#include "media/ImageSequence.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#ifdef FR_HAVE_AUDIO
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QtAudio>
#include <QIODevice>
#include <QMediaDevices>
#include <QHash>
#include <QTimer>

#include <mutex>
#include <vector>
#endif

namespace fr {

#ifdef FR_HAVE_AUDIO
namespace {

// Feeds the sound card from an AudioMix, from a chosen audio frame on.
//
// Playing runs on without end. Scrubbing plays a burst and then silence, so the sink keeps running
// and the next burst only moves the read position: restarting the sink for every step blocks the
// UI thread (about one frame's time with PulseAudio) and drops about half of the bursts.
class PcmDevice : public QIODevice {
public:
    explicit PcmDevice(const AudioMix* mix) : mix_(mix) {}

    void startPlaying(qint64 startFrame) {
        close();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            position_ = startFrame;
            burstEnd_ = -1;
        }
        open(QIODevice::ReadOnly);
    }
    void startScrubbing(qint64 startFrame, qint64 endFrame) {
        close();
        scrubTo(startFrame, endFrame);
        open(QIODevice::ReadOnly);
    }
    // Moves the burst while the sink keeps reading.
    void scrubTo(qint64 startFrame, qint64 endFrame) {
        std::lock_guard<std::mutex> lock(mutex_);
        position_ = startFrame;
        burstEnd_ = endFrame;
    }
    void stop() { close(); }

    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override {
        return (qint64(1) << 20) * AudioTrack::kBytesPerFrame + QIODevice::bytesAvailable();
    }

protected:
    qint64 readData(char* data, qint64 maxSize) override {
        const qint64 frames = maxSize / AudioTrack::kBytesPerFrame;
        if (frames <= 0) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        const qint64 sounding = burstEnd_ < 0 ? frames : std::clamp<qint64>(burstEnd_ - position_, 0, frames);
        const qint64 bytes = frames * AudioTrack::kBytesPerFrame;
        const qint64 soundBytes = sounding * AudioTrack::kBytesPerFrame;
        if (sounding > 0) {
            const QByteArray chunk = mix_->read(position_, sounding);
            std::memcpy(data, chunk.constData(), static_cast<std::size_t>(chunk.size()));
        }
        std::memset(data + soundBytes, 0, static_cast<std::size_t>(bytes - soundBytes));  // after the burst
        position_ += sounding;
        return bytes;
    }
    qint64 writeData(const char*, qint64) override { return -1; }

private:
    const AudioMix* mix_;
    std::mutex mutex_;
    qint64 position_ = 0;
    qint64 burstEnd_ = -1;  // -1 while playing
};

constexpr int kScrubIdleMs = 1000;  // the sink is released this long after the last scrub step

}  // namespace

struct AudioController::Impl {
    explicit Impl(Session& s) : session(s), device(&mix) {}

    Session& session;
    QHash<QString, std::shared_ptr<AudioTrack>> tracks;  // by file, for the segments of `mix`
    AudioMix mix;
    QString mixSignature;  // what `mix` was built from; unchanged means nothing to reload
    PcmDevice device;
    std::unique_ptr<QAudioSink> sink;
    bool muted = false;
    double volume = 1.0;
    bool scrub = true;
    int bufferMs = 100;
    bool playing = false;
    bool scrubbing = false;  // the sink runs a scrub stream
    QTimer scrubIdle;
    int lastFrame = -1;

    void ensureSink() {
        if (sink) return;
        const QAudioDevice output = QMediaDevices::defaultAudioOutput();
        if (output.isNull()) return;  // no sound card (a headless machine)
        QAudioFormat format;
        format.setSampleRate(AudioTrack::kSampleRate);
        format.setChannelCount(AudioTrack::kChannels);
        format.setSampleFormat(QAudioFormat::Int16);
        sink = std::make_unique<QAudioSink>(output, format);
        sink->setBufferSize(bufferMs * (AudioTrack::kSampleRate / 1000) * AudioTrack::kBytesPerFrame);
        applyVolume();
    }

    // The slider is how loud it sounds, so it is mapped from a perceived (logarithmic) scale.
    void applyVolume() {
        if (!sink) return;
        sink->setVolume(muted ? 0.0
                              : QtAudio::convertVolume(static_cast<float>(volume), QtAudio::LogarithmicVolumeScale,
                                                       QtAudio::LinearVolumeScale));
    }

    void stopSink() {
        scrubIdle.stop();
        scrubbing = false;
        if (sink) sink->stop();
        device.stop();
    }

    // While the sound plays it is the clock, and the picture keeps up with it; otherwise playback
    // waits for each frame to be decoded.
    void setPlaying(bool on) {
        playing = on;
        session.playback().setRealtime(on);
    }

    bool shouldSound() const {
        const PlaybackController& playback = session.playback();
        return playback.isPlaying() && playback.isForward() && std::fabs(playback.speed() - 1.0) < 1e-6;
    }

    // A viewer frame as an audio frame of the mix.
    qint64 audioFrame(double viewerFrame) const {
        return static_cast<qint64>(
            std::llround(viewerFrame / std::max(1.0, session.playback().fps()) * AudioTrack::kSampleRate));
    }

    // Where the playhead is in the mix, in audio frames (the sources' delays are in the segments).
    qint64 audioPosition() const {
        PlaybackController& playback = session.playback();
        return audioFrame(playback.clockFrame() - session.viewerOffset(session.activeViewer()));
    }

    // Rebuilds the mix for what the active viewer shows: a source plays its file from frame 0; a
    // timeline plays each clip's file, trimmed to the clip, where the clip sits. Image clips are
    // silent. Clips are mapped at the timeline's frame rate.
    void reload() {
        struct Part {
            QString path;
            qint64 start = 0;
            qint64 length = -1;
            qint64 trackStart = 0;
        };
        std::vector<Part> parts;
        const auto playable = [](const QString& path) {
            return !path.isEmpty() && !isSequencePath(path) && !isImageFile(path);
        };
        const auto delayFrames = [this](const QString& sourceId) -> qint64 {
            const Source* source = session.project().findSource(sourceId);
            return source ? std::llround(source->audioDelayMs / 1000.0 * AudioTrack::kSampleRate) : 0;
        };
        const QString kind = session.viewerTargetKind();
        if (kind == QLatin1String("source")) {
            const QString id = session.viewerTargetId();
            const QString path = session.sourcePath(id);
            if (playable(path)) parts.push_back(Part{path, 0, -1, -delayFrames(id)});
        } else if (kind == QLatin1String("timeline")) {
            int at = 0;
            for (const ClipRef& clip : session.viewerClips()) {
                const QString path = session.sourcePath(clip.sourceId);
                if (playable(path) && clip.count > 0) {
                    const qint64 start = audioFrame(at);
                    parts.push_back(Part{path, start, audioFrame(at + clip.count) - start,
                                         audioFrame(clip.first) - delayFrames(clip.sourceId)});
                }
                at += std::max(0, clip.count);
            }
        }

        QString signature = kind + QLatin1Char('|') + QString::number(session.playback().fps());
        for (const Part& part : parts) {
            signature += QStringLiteral("|%1,%2,%3,%4").arg(part.path).arg(part.start).arg(part.length).arg(part.trackStart);
        }
        if (signature == mixSignature) return;  // moving between clips of the same timeline: seamless
        mixSignature = signature;

        stopSink();
        setPlaying(false);
        QHash<QString, std::shared_ptr<AudioTrack>> kept;
        std::vector<AudioMix::Segment> segments;
        for (const Part& part : parts) {
            std::shared_ptr<AudioTrack> track = kept.value(part.path);
            if (!track) track = tracks.value(part.path);
            if (!track) {
                track = std::make_shared<AudioTrack>();
                track->open(part.path);  // decodes in the background; silent until decoded
            }
            kept.insert(part.path, track);
            segments.push_back(AudioMix::Segment{track, part.start, part.length, part.trackStart});
        }
        mix.setSegments(std::move(segments));
        tracks = std::move(kept);  // files no longer used stop decoding and are dropped
        if (shouldSound()) startPlayback();
    }

    void startPlayback() {
        if (!mix.hasAudio() || muted) return;
        ensureSink();
        if (!sink) return;
        scrubIdle.stop();
        scrubbing = false;
        sink->stop();
        device.startPlaying(audioPosition());
        sink->start(&device);
        setPlaying(true);
    }

    void scrubHere() {
        if (!mix.hasAudio() || muted || !scrub) return;
        ensureSink();
        if (!sink) return;
        const qint64 burst = std::max<qint64>(
            1200, static_cast<qint64>(AudioTrack::kSampleRate / std::max(1.0, session.playback().fps())));
        const qint64 start = audioPosition();
        if (scrubbing && sink->state() == QtAudio::ActiveState) {
            device.scrubTo(start, start + burst);
        } else {
            sink->stop();
            device.startScrubbing(start, start + burst);
            sink->start(&device);
            scrubbing = true;
        }
        scrubIdle.start();
    }
};

AudioController::AudioController(Session& session, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>(session)) {
    Impl* d = impl_.get();
    PlaybackController* playback = &session.playback();

    d->scrubIdle.setSingleShot(true);
    d->scrubIdle.setInterval(kScrubIdleMs);
    connect(&d->scrubIdle, &QTimer::timeout, this, [d] {
        if (d->scrubbing && !d->playing) d->stopSink();
    });

    connect(&session, &Session::activeSourceChanged, this, [d] { d->reload(); });
    connect(&session, &Session::activeViewerChanged, this, [d] { d->reload(); });
    connect(&session, &Session::projectReplaced, this, [d] { d->reload(); });
    // Timeline edits, cuts and audio delays change the mix; anything else leaves it as it is.
    connect(&session, &Session::projectChanged, this, [d] { d->reload(); });
    connect(&session, &Session::timelinesChanged, this, [d] { d->reload(); });

    // Sound plays forwards at normal speed only; any change of that starts or stops it.
    const auto follow = [d, playback] {
        if (d->shouldSound()) {
            d->lastFrame = playback->clockFrame();
            d->startPlayback();
        } else {
            d->stopSink();
            d->setPlaying(false);
        }
    };
    connect(playback, &PlaybackController::playingChanged, this, follow);
    connect(playback, &PlaybackController::speedChanged, this, follow);
    connect(playback, &PlaybackController::looped, this, [d, playback] {
        if (!d->playing) return;
        d->lastFrame = playback->clockFrame();
        d->startPlayback();
    });
    connect(playback, &PlaybackController::frameChanged, this, [d, playback](int) {
        // Stepped playback holds keys while the clock runs on; the sound keeps to the clock.
        const int frame = playback->clockFrame();
        if (d->playing && !playback->isForward()) {  // ping-pong turned round: no sound backwards
            d->stopSink();
            d->setPlaying(false);
        } else if (!d->playing && d->shouldSound() && !d->muted && d->mix.hasAudio()) {
            d->lastFrame = frame;  // ping-pong turned forwards again
            d->startPlayback();
            return;
        }
        if (d->playing) {
            // The sound is the clock: only a loop or a jump needs the sound to follow the playhead.
            const bool jumped = frame < d->lastFrame ||
                                (!playback->isStepped() &&
                                 frame > d->lastFrame + std::max(8, static_cast<int>(playback->fps())));
            if (jumped) d->startPlayback();
            d->lastFrame = frame;
            return;
        }
        // While playing backwards, faster or slower than normal, playback is silent; only stepping
        // and seeking while paused scrub.
        if (!playback->isPlaying()) d->scrubHere();
    });
}

AudioController::~AudioController() {
    if (impl_) impl_->stopSink();
}

bool AudioController::available() {
#ifdef FR_HAVE_FFMPEG
    return true;
#else
    return false;
#endif
}

void AudioController::setMuted(bool muted) {
    impl_->muted = muted;
    impl_->applyVolume();
    if (muted) {
        impl_->stopSink();
        impl_->setPlaying(false);
    } else if (impl_->shouldSound()) {
        impl_->startPlayback();  // unmuted while playing: the sound comes back at once
    }
}

bool AudioController::muted() const {
    return impl_->muted;
}

void AudioController::setVolume(double volume) {
    impl_->volume = std::clamp(volume, 0.0, 1.0);
    impl_->applyVolume();
}

double AudioController::volume() const {
    return impl_->volume;
}

void AudioController::setScrubEnabled(bool enabled) {
    impl_->scrub = enabled;
}

bool AudioController::scrubEnabled() const {
    return impl_->scrub;
}

void AudioController::setBufferMs(int milliseconds) {
    milliseconds = std::clamp(milliseconds, 20, 1000);
    if (milliseconds == impl_->bufferMs) return;
    impl_->bufferMs = milliseconds;
    impl_->stopSink();
    impl_->sink.reset();  // rebuilt with the new buffer
    impl_->setPlaying(false);
    if (impl_->shouldSound()) impl_->startPlayback();
}

bool AudioController::hasTrack() const {
    return impl_->mix.hasAudio();
}

#else  // no Qt Multimedia: the controller does nothing

struct AudioController::Impl {
    explicit Impl(Session&) {}
    bool muted = false;
    double volume = 1.0;
    bool scrub = true;
};

AudioController::AudioController(Session& session, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>(session)) {}

AudioController::~AudioController() = default;

bool AudioController::available() {
    return false;
}

void AudioController::setMuted(bool muted) { impl_->muted = muted; }
bool AudioController::muted() const { return impl_->muted; }
void AudioController::setVolume(double volume) { impl_->volume = std::clamp(volume, 0.0, 1.0); }
double AudioController::volume() const { return impl_->volume; }
void AudioController::setScrubEnabled(bool enabled) { impl_->scrub = enabled; }
bool AudioController::scrubEnabled() const { return impl_->scrub; }
void AudioController::setBufferMs(int) {}
bool AudioController::hasTrack() const { return false; }

#endif

}  // namespace fr
