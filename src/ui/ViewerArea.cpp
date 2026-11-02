#include "ui/ViewerArea.h"

#include "ui/ViewerWidget.h"

#include <QSplitter>
#include <QVBoxLayout>

#include <algorithm>

namespace fr {

ViewerArea::ViewerArea(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    splitter_ = new QSplitter(Qt::Horizontal, this);
    splitter_->setChildrenCollapsible(false);
    layout->addWidget(splitter_);

    for (int i = 0; i < 2; ++i) {
        viewers_[i] = new ViewerWidget(splitter_);
        splitter_->addWidget(viewers_[i]);
        connect(viewers_[i], &ViewerWidget::activated, this, [this, i] { emit viewerActivated(i); });
    }
    viewers_[1]->hide();
    updateHighlights();
}

ViewerWidget* ViewerArea::viewer(int index) const {
    return viewers_[std::clamp(index, 0, 1)];
}

void ViewerArea::setLayoutMode(const QString& layout) {
    twoOpen_ = layout == QLatin1String("horizontal") || layout == QLatin1String("vertical");
    // The split layouts show both pictures in the first viewer, divided by a wipe line.
    const bool split = layout.startsWith(QLatin1String("split_"));
    viewers_[0]->setSplit(split, layout == QLatin1String("split_vertical") ? Qt::Vertical : Qt::Horizontal);
    splitter_->setOrientation(layout == QLatin1String("vertical") ? Qt::Vertical : Qt::Horizontal);
    viewers_[1]->setVisible(twoOpen_);
    if (twoOpen_) splitter_->setSizes({1, 1});
    updateHighlights();
}

void ViewerArea::setActiveViewer(int index) {
    active_ = std::clamp(index, 0, 1);
    updateHighlights();
}

void ViewerArea::updateHighlights() {
    for (int i = 0; i < 2; ++i) viewers_[i]->setActiveHighlight(twoOpen_ && i == active_);
}

}  // namespace fr
