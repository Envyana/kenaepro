#pragma once

#include <QObject>

#include <memory>

namespace fr {

class Session;

// Plays the sound of the source in the active viewer in step with the playhead.
//
// - Sound plays while the playhead runs forward at normal speed; backwards, faster or slower, it
//   is silent. A timeline plays each clip's sound where the clip sits (image clips are silent).
// - Stepping or scrubbing a frame plays a short burst of that frame's sound ("scrub"), if enabled.
// - A source's audio delay shifts the sound against the picture.
//
// Needs Qt Multimedia and FFmpeg; without either the controller is inert and available() is false.
class AudioController : public QObject {
public:
    explicit AudioController(Session& session, QObject* parent = nullptr);
    ~AudioController() override;

    static bool available();

    void setMuted(bool muted);
    bool muted() const;
    void setVolume(double volume);  // 0..1
    double volume() const;
    void setScrubEnabled(bool enabled);
    bool scrubEnabled() const;
    void setBufferMs(int milliseconds);
    bool hasTrack() const;  // the active source has sound

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fr
