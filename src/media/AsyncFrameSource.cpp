#include "media/AsyncFrameSource.h"

#include <QMetaObject>

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <utility>

namespace fr {
namespace {

constexpr qint64 kCacheBudgetBytes = 256LL * 1024 * 1024;
constexpr int kMinCachedFrames = 4;
constexpr int kMaxCachedFrames = 4000;
constexpr int kMaxWorkers = 6;

// Frames are kept as 32-bit pixels whatever the file held (16-bit PNG, TIFF), so the cache budget
// holds and the viewer can draw them without converting on the UI thread.
QImage normalised(QImage image) {
    if (image.isNull()) return image;
    const QImage::Format format = image.hasAlphaChannel() ? QImage::Format_ARGB32_Premultiplied : QImage::Format_RGB32;
    if (image.format() != format) image = image.convertToFormat(format);
    return image;
}

}  // namespace

int AsyncFrameSource::limitFor(qint64 budgetBytes, QSize size) {
    const qint64 frameBytes = std::max<qint64>(1, static_cast<qint64>(size.width()) * size.height() * 4);
    const qint64 budget = budgetBytes > 0 ? budgetBytes : kCacheBudgetBytes;
    return static_cast<int>(std::clamp<qint64>(budget / frameBytes, kMinCachedFrames, kMaxCachedFrames));
}

AsyncFrameSource::AsyncFrameSource(std::unique_ptr<FrameSource> source, int initialIndex,
                                   QObject* parent, int maxCachedFrames, qint64 budgetBytes)
    : QObject(parent), source_(std::move(source)) {
    frameCount_ = source_->frameCount();
    fps_ = source_->fps();
    size_ = source_->size();
    frameNumbers_ = source_->frameNumbers();

    cacheLimit_ = maxCachedFrames > 0 ? std::max(kMinCachedFrames, maxCachedFrames) : limitFor(budgetBytes, size_);

    wanted_ = std::clamp(initialIndex, 0, std::max(0, frameCount_ - 1));
    if (frameCount_ > 0) {
        const QImage first = normalised(source_->frame(wanted_));
        if (!first.isNull()) cache_.insert(wanted_, first);
    }

    int workers = 1;
    if (source_->concurrentFrames()) {
        workers = std::clamp(static_cast<int>(std::thread::hardware_concurrency()) - 1, 1, kMaxWorkers);
    }
    for (int i = 0; i < workers; ++i) threads_.emplace_back([this] { run(); });
}

AsyncFrameSource::~AsyncFrameSource() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    for (std::thread& thread : threads_) thread.join();
}

void AsyncFrameSource::request(int index, int direction) {
    if (frameCount_ <= 0) return;
    index = std::clamp(index, 0, frameCount_ - 1);
    const int dir = direction >= 0 ? 1 : -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index == wanted_ && dir == direction_) return;
        wanted_ = index;
        direction_ = dir;
    }
    wake_.notify_all();
}

void AsyncFrameSource::setPlaybackWindow(int start, int end, bool wraps) {
    if (frameCount_ <= 0) return;
    start = std::clamp(start, 0, frameCount_ - 1);
    end = std::clamp(end, start, frameCount_ - 1);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (start == rangeStart_ && end == rangeEnd_ && wraps == wraps_) return;
        rangeStart_ = start;
        rangeEnd_ = end;
        wraps_ = wraps;
    }
    wake_.notify_all();
}

void AsyncFrameSource::setBudget(qint64 budgetBytes) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cacheLimit_ = limitFor(budgetBytes, size_);
        evictLocked();
    }
    wake_.notify_all();
}

bool AsyncFrameSource::isReady(int index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.contains(index) || failed_.contains(index);
}

QImage AsyncFrameSource::exactFrame(int index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.value(index);
}

QImage AsyncFrameSource::nearestFrame(int index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cache_.isEmpty()) return {};
    const auto above = cache_.lowerBound(index);  // first key >= index
    if (above != cache_.cend() && above.key() == index) return above.value();
    if (above == cache_.cbegin()) return above.value();
    const auto below = std::prev(above);
    if (above == cache_.cend()) return below.value();
    return (index - below.key() <= above.key() - index) ? below.value() : above.value();
}

int AsyncFrameSource::cachedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(cache_.size());
}

// The requested frame, then the frames that will play after it, then a few that played before it.
// Inside a wrapping range the order goes round the range, as playback will. When the whole range
// fits in the cache the window is the whole range, so nothing in it is ever dropped.
QVector<int> AsyncFrameSource::windowLocked() const {
    QVector<int> window;
    if (frameCount_ <= 0) return window;
    const int first = rangeEnd_ >= 0 ? rangeStart_ : 0;
    const int last = rangeEnd_ >= 0 ? rangeEnd_ : frameCount_ - 1;
    const bool inRange = wanted_ >= first && wanted_ <= last;
    const bool wrap = wraps_ && inRange;
    const int rangeLength = last - first + 1;

    int ahead = std::max(1, cacheLimit_ * 2 / 3);
    int behind = std::max(1, cacheLimit_ / 4);
    if (wrap && rangeLength <= cacheLimit_) {
        ahead = rangeLength - 1;
        behind = 0;
    }
    const auto step = [&](int i, int dir) {
        i += dir;
        if (wrap) {
            if (i > last) i = first;
            if (i < first) i = last;
        }
        return i;
    };
    window.reserve(ahead + behind + 1);
    window.push_back(wanted_);
    QSet<int> seen{wanted_};
    for (int k = 0, i = wanted_; k < ahead; ++k) {
        i = step(i, direction_);
        if (i < 0 || i >= frameCount_ || seen.contains(i)) break;
        seen.insert(i);
        window.push_back(i);
    }
    for (int k = 0, i = wanted_; k < behind; ++k) {
        i = step(i, -direction_);
        if (i < 0 || i >= frameCount_ || seen.contains(i)) break;
        seen.insert(i);
        window.push_back(i);
    }
    return window;
}

// Next frame to decode, or -1 when everything worth having is cached or being decoded.
int AsyncFrameSource::nextToDecodeLocked() const {
    for (const int i : windowLocked()) {
        if (!cache_.contains(i) && !failed_.contains(i) && !inFlight_.contains(i)) return i;
    }
    return -1;
}

// Drops frames outside the window first, the farthest from the requested frame first; then, if the
// cache shrank below the window, the least wanted end of the window.
void AsyncFrameSource::evictLocked() {
    if (cache_.size() <= cacheLimit_) return;
    const QVector<int> window = windowLocked();
    const QSet<int> keep(window.cbegin(), window.cend());
    while (cache_.size() > cacheLimit_) {
        int farthest = -1;
        int farthestDistance = -1;
        for (auto it = cache_.cbegin(); it != cache_.cend(); ++it) {
            if (keep.contains(it.key())) continue;
            const int distance = std::abs(it.key() - wanted_);
            if (distance > farthestDistance) {
                farthestDistance = distance;
                farthest = it.key();
            }
        }
        if (farthest < 0) {
            for (auto it = window.crbegin(); it != window.crend(); ++it) {
                if (cache_.contains(*it)) {
                    farthest = *it;
                    break;
                }
            }
        }
        if (farthest < 0) break;
        cache_.remove(farthest);
    }
}

void AsyncFrameSource::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        int next = -1;
        wake_.wait(lock, [this, &next] { return stop_ || (next = nextToDecodeLocked()) >= 0; });
        if (stop_) return;

        inFlight_.insert(next);
        lock.unlock();
        const QImage image = normalised(source_->frame(next));  // the slow part; no lock held
        lock.lock();
        inFlight_.remove(next);

        if (image.isNull()) {
            failed_.insert(next);
            continue;
        }
        cache_.insert(next, image);
        evictLocked();
        QMetaObject::invokeMethod(this, [this, next] { emit frameReady(next); }, Qt::QueuedConnection);
        wake_.notify_all();  // another worker may be waiting for the window to move on
    }
}

}  // namespace fr
