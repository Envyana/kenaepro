#pragma once

#include "media/FrameSource.h"

#include <QImage>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QSize>
#include <QVector>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace fr {

// Decodes frames on worker threads so the UI never waits for a decoder.
//
// The UI thread only calls request() and reads frames back with exactFrame() or nearestFrame();
// none of them blocks on decoding. The workers keep a cache of decoded frames and fill it in
// playing order: the requested frame, then the frames after it in the playing direction (wrapping
// round the playback range when it loops), then a few before it. Frames outside that window are
// dropped first when the cache is full, so a looping range that fits in the cache is decoded once
// and then plays from memory.
//
// Sources whose frames are separate files (image sequences) are decoded by several workers at
// once; a video stream has one.
//
// While the requested frame is not ready, nearestFrame() returns the closest decoded one, so the
// picture is a little stale instead of blank. frameReady() tells the UI when something new
// arrived. The worker is the only user of the wrapped FrameSource after construction.
//
// The worker uses the standard thread primitives rather than QThread/QMutex so that
// ThreadSanitizer, which does not understand Qt's own locking, can check this class.
class AsyncFrameSource : public QObject {
    Q_OBJECT
public:
    // Takes over `source` and decodes `initialIndex` before returning (the only blocking call, so
    // there is something to show at once). `maxCachedFrames` of 0 sizes the cache from a memory
    // budget (`budgetBytes`, or 256 MB when that is 0); the cache never holds fewer than 4 frames.
    AsyncFrameSource(std::unique_ptr<FrameSource> source, int initialIndex, QObject* parent = nullptr,
                     int maxCachedFrames = 0, qint64 budgetBytes = 0);
    // Stops the worker. Waits for the frame being decoded right now, so it can take as long as
    // one decode.
    ~AsyncFrameSource() override;

    int frameCount() const { return frameCount_; }
    double fps() const { return fps_; }
    QSize size() const { return size_; }
    int cacheLimit() const { return cacheLimit_; }
    // The number in the file name of a frame of an image sequence; -1 when there is none.
    int imageNumber(int index) const {
        return (index >= 0 && index < frameNumbers_.size()) ? frameNumbers_[index] : -1;
    }

    // Show `index` as soon as possible and prefetch beyond it in `direction` (+1 or -1).
    void request(int index, int direction);
    // The frames that play, inclusive, and whether playback wraps round them (loop, ping-pong).
    void setPlaybackWindow(int start, int end, bool wraps);
    void setBudget(qint64 budgetBytes);  // resizes the cache
    bool isReady(int index) const;  // decoded, or known to be undecodable (waiting would not help)
    int workerCount() const { return static_cast<int>(threads_.size()); }
    QImage exactFrame(int index) const;    // null unless that frame is decoded
    QImage nearestFrame(int index) const;  // the frame itself, else the closest decoded one, else null
    int cachedCount() const;

signals:
    // A frame was decoded and cached. Delivered on the thread that owns this object.
    void frameReady(int index);

private:
    void run();
    QVector<int> windowLocked() const;  // the frames worth keeping, most wanted first
    int nextToDecodeLocked() const;
    void evictLocked();
    static int limitFor(qint64 budgetBytes, QSize size);

    std::unique_ptr<FrameSource> source_;
    int frameCount_ = 0;
    double fps_ = 0.0;
    QSize size_;
    int cacheLimit_ = 4;
    QVector<int> frameNumbers_;  // read once at construction, never changed afterwards

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    QMap<int, QImage> cache_;
    QSet<int> failed_;    // frames the decoder could not produce; not retried
    QSet<int> inFlight_;  // being decoded by a worker right now
    int wanted_ = 0;
    int direction_ = 1;
    int rangeStart_ = 0;
    int rangeEnd_ = -1;  // -1: the whole source
    bool wraps_ = false;
    bool stop_ = false;
    std::vector<std::thread> threads_;
};

}  // namespace fr
