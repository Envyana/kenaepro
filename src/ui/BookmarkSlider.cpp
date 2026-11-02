#include "ui/BookmarkSlider.h"

#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionSlider>

#include <algorithm>

namespace fr {

BookmarkSlider::BookmarkSlider(QWidget* parent) : QSlider(Qt::Horizontal, parent) {
    setMinimumHeight(30);
}

void BookmarkSlider::setMarks(const QVector<int>& frames, const QVector<RangeBookmark>& ranges) {
    if (frames == frames_ && ranges == ranges_) return;
    frames_ = frames;
    ranges_ = ranges;
    update();
}

void BookmarkSlider::setAnnotationMarks(const QVector<int>& frames) {
    if (frames == annotations_) return;
    annotations_ = frames;
    update();
}

void BookmarkSlider::setPlaybackRange(int start, int end) {
    if (start == rangeStart_ && end == rangeEnd_) return;
    rangeStart_ = start;
    rangeEnd_ = end;
    update();
}

int BookmarkSlider::xForValue(int value) const {
    QStyleOptionSlider option;
    initStyleOption(&option);
    const QRect groove = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
    const QRect handle = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
    const int span = std::max(1, groove.width() - handle.width());
    const int position =
        QStyle::sliderPositionFromValue(minimum(), maximum(), value, span, option.upsideDown);
    return groove.left() + position + handle.width() / 2;
}

void BookmarkSlider::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && maximum() > minimum()) {
        QStyleOptionSlider option;
        initStyleOption(&option);
        const QRect handle = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
        if (!handle.contains(event->position().toPoint())) {
            const QRect groove = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
            const int span = std::max(1, groove.width() - handle.width());
            const int x = qRound(event->position().x()) - groove.left() - handle.width() / 2;
            setValue(QStyle::sliderValueFromPosition(minimum(), maximum(), std::clamp(x, 0, span), span,
                                                     option.upsideDown));
        }
    }
    QSlider::mousePressEvent(event);  // the handle is under the pointer now, so it drags from here
}

void BookmarkSlider::paintEvent(QPaintEvent* event) {
    QSlider::paintEvent(event);
    const bool narrowed = rangeEnd_ >= 0 && (rangeStart_ > minimum() || rangeEnd_ < maximum());
    if (frames_.isEmpty() && ranges_.isEmpty() && annotations_.isEmpty() && !narrowed) return;

    constexpr int kRangeHeight = 6;
    constexpr int kTickHeight = 12;
    constexpr int kAnnotationHeight = 6;
    const int bottom = height();
    QPainter painter(this);
    if (narrowed) {  // dim what plays outside the range
        const QColor shade(0, 0, 0, 110);
        const int from = xForValue(rangeStart_);
        const int to = xForValue(rangeEnd_);
        if (rangeStart_ > minimum()) painter.fillRect(QRect(0, 0, from, bottom), shade);
        if (rangeEnd_ < maximum()) painter.fillRect(QRect(to + 1, 0, width() - to - 1, bottom), shade);
    }
    // The range timeline shows only part of the frames: marks outside it are left out.
    const auto inside = [this](int frame) { return frame >= minimum() && frame <= maximum(); };
    for (const int frame : annotations_) {
        if (!inside(frame)) continue;
        painter.fillRect(QRect(xForValue(frame) - 1, 0, 3, kAnnotationHeight), annotationColor());
    }
    for (const RangeBookmark& range : ranges_) {  // bars first, so a tick is never hidden by one
        if (range.end < minimum() || range.start > maximum()) continue;
        const int from = xForValue(std::max(range.start, minimum()));
        const int to = xForValue(std::min(range.end, maximum()));
        painter.fillRect(QRect(from, bottom - kRangeHeight, std::max(1, to - from + 1), kRangeHeight),
                         rangeColor());
    }
    for (const int frame : frames_) {
        if (!inside(frame)) continue;
        painter.fillRect(QRect(xForValue(frame) - 1, bottom - kTickHeight, 3, kTickHeight), frameColor());
    }
}

}  // namespace fr
