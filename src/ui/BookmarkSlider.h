#pragma once

#include "core/ProjectModel.h"

#include <QColor>
#include <QSlider>
#include <QVector>

namespace fr {

// The frame slider, with the active source's bookmarks drawn on top of it: a yellow tick for each
// frame bookmark and a blue bar for each range bookmark. Frames with drawings or notes get a green
// tick along the top edge, and the frames outside the playback range are dimmed.
class BookmarkSlider : public QSlider {
    Q_OBJECT
public:
    explicit BookmarkSlider(QWidget* parent = nullptr);

    void setMarks(const QVector<int>& frames, const QVector<RangeBookmark>& ranges);
    void setAnnotationMarks(const QVector<int>& frames);
    // The playback range, inclusive; the whole slider when it covers every frame.
    void setPlaybackRange(int start, int end);

    // Where the middle of the handle sits for `value`, in widget pixels.
    int xForValue(int value) const;

    static QColor frameColor() { return QColor(255, 200, 0); }
    static QColor rangeColor() { return QColor(70, 130, 230); }
    static QColor annotationColor() { return QColor(90, 210, 120); }

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;  // a click jumps there, then drags

private:
    QVector<int> frames_;
    QVector<RangeBookmark> ranges_;
    QVector<int> annotations_;
    int rangeStart_ = 0;
    int rangeEnd_ = -1;  // -1: no range set
};

}  // namespace fr
