#include "media/ClipSequenceSource.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace fr {

ClipSequenceSource::ClipSequenceSource(std::vector<Entry> entries) : entries_(std::move(entries)) {
    int start = 0;
    for (const Entry& entry : entries_) {
        starts_.push_back(start);
        start += std::max(0, entry.count);
    }
    total_ = start;
}

double ClipSequenceSource::fps() const {
    for (const Entry& entry : entries_) {
        if (entry.source && entry.source->fps() > 0.0) return entry.source->fps();
    }
    return 0.0;
}

QSize ClipSequenceSource::size() const {
    if (entries_.empty() || !entries_.front().source) return QSize();
    return entries_.front().source->size();
}

QImage ClipSequenceSource::frame(int index) {
    if (index < 0 || index >= total_) return {};
    // The run that contains `index` is the last one that starts at or before it.
    const auto after = std::upper_bound(starts_.begin(), starts_.end(), index);
    const std::size_t run = static_cast<std::size_t>(std::distance(starts_.begin(), after)) - 1;
    Entry& entry = entries_[run];
    if (!entry.source) return {};
    return entry.source->frame(entry.first + (index - starts_[run]));
}

bool ClipSequenceSource::concurrentFrames() const {
    return std::all_of(entries_.begin(), entries_.end(),
                       [](const Entry& entry) { return entry.source && entry.source->concurrentFrames(); });
}

QVector<int> ClipSequenceSource::frameNumbers() const {
    QVector<int> numbers;
    bool any = false;
    for (const Entry& entry : entries_) {
        const QVector<int> own = entry.source ? entry.source->frameNumbers() : QVector<int>();
        for (int i = 0; i < entry.count; ++i) {
            const int at = entry.first + i;
            const int number = (at >= 0 && at < own.size()) ? own[at] : -1;
            if (number >= 0) any = true;
            numbers.push_back(number);
        }
    }
    return any ? numbers : QVector<int>();
}

}  // namespace fr
