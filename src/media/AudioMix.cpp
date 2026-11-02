#include "media/AudioMix.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace fr {

void AudioMix::setSegments(std::vector<Segment> segments) {
    std::lock_guard<std::mutex> lock(mutex_);
    segments_ = std::move(segments);
}

bool AudioMix::hasAudio() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(segments_.begin(), segments_.end(),
                       [](const Segment& s) { return s.track && s.track->hasAudio(); });
}

QByteArray AudioMix::read(qint64 startFrame, qint64 frames) const {
    if (frames <= 0) return {};
    QByteArray out(static_cast<qsizetype>(frames * AudioTrack::kBytesPerFrame), '\0');
    const qint64 endFrame = startFrame + frames;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const Segment& segment : segments_) {
        if (!segment.track || !segment.track->hasAudio()) continue;
        const qint64 segmentEnd =
            segment.length < 0 ? std::numeric_limits<qint64>::max() : segment.start + segment.length;
        const qint64 from = std::max(startFrame, segment.start);
        const qint64 to = std::min(endFrame, segmentEnd);
        if (to <= from) continue;
        const QByteArray chunk = segment.track->read(segment.trackStart + (from - segment.start), to - from);
        std::memcpy(out.data() + (from - startFrame) * AudioTrack::kBytesPerFrame, chunk.constData(),
                    static_cast<std::size_t>(chunk.size()));
    }
    return out;
}

}  // namespace fr
