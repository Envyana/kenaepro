#pragma once

#include <QString>
#include <QWidget>

class QSplitter;

namespace fr {

class ViewerWidget;

// The central area: one viewer, or two side by side or stacked. The second viewer always exists
// (so it keeps its view settings) but is hidden in the single layout.
class ViewerArea : public QWidget {
    Q_OBJECT
public:
    explicit ViewerArea(QWidget* parent = nullptr);

    ViewerWidget* viewer(int index) const;  // 0 or 1
    // "single", "horizontal" (side by side), "vertical" (stacked), or "split_horizontal" /
    // "split_vertical" (both pictures in one viewer, A | B or A over B, with a movable divider).
    void setLayoutMode(const QString& layout);
    // Marks the viewer that edits apply to; the mark only shows when two viewers are open.
    void setActiveViewer(int index);

signals:
    void viewerActivated(int index);  // the user pressed a mouse button in a viewer

private:
    void updateHighlights();

    QSplitter* splitter_ = nullptr;
    ViewerWidget* viewers_[2] = {nullptr, nullptr};
    int active_ = 0;
    bool twoOpen_ = false;
};

}  // namespace fr
