#pragma once

#include "media/AudioTrack.h"

#include <QByteArray>

#include <memory>
#include <mutex>
#include <vector>

namespace fr {

// The sound of what a viewer shows, as one run of audio frames: one source, or the clips of a
// timeline back to back. Each segment places part of a track at a position; where no segment is,
// it is silent (image clips, clips without sound, gaps).
class AudioMix {
public:
    struct Segment {
        std::shared_ptr<const AudioTrack> track;
        qint64 start = 0;       // first audio frame of the mix that the segment covers
        qint64 length = -1;     // audio frames it covers; -1 runs on without end
        qint64 trackStart = 0;  // the track's audio frame at `start` (the source's delay included)
    };

    void setSegments(std::vector<Segment> segments);
    void clear() { setSegments({}); }
    bool hasAudio() const;  // some segment has sound

    // Exactly `frames` audio frames from `startFrame`; silence where no segment has sound.
    QByteArray read(qint64 startFrame, qint64 frames) const;

private:
    mutable std::mutex mutex_;
    std::vector<Segment> segments_;
};

}  // namespace fr
