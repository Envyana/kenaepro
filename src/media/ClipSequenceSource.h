#pragma once

#include "media/FrameSource.h"

#include <memory>
#include <vector>

namespace fr {

// Plays several runs of frames, taken from different sources, back to back as one source. A
// plain source is the one-run case, a timeline is several runs. The runs' decoders are already
// open; frames are decoded lazily by the single thread that uses this object.
class ClipSequenceSource final : public FrameSource {
public:
    struct Entry {
        std::unique_ptr<FrameSource> source;
        int first = 0;  // first frame of `source` that is used
        int count = 0;  // how many frames, from `first`
    };

    explicit ClipSequenceSource(std::vector<Entry> entries);

    bool open(const QString&, QString*) override { return true; }  // entries arrive open
    int frameCount() const override { return total_; }
    double fps() const override;   // the first run that has a rate of its own; 0 if none
    QSize size() const override;   // of the first run
    QImage frame(int index) override;
    // Image file numbers of the frames; -1 where a frame has none; empty when no frame has one.
    QVector<int> frameNumbers() const override;
    bool concurrentFrames() const override;  // when every run's source allows it

private:
    std::vector<Entry> entries_;
    std::vector<int> starts_;  // index of each run's first frame in the whole
    int total_ = 0;
};

}  // namespace fr
