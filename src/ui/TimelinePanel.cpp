#include "ui/TimelinePanel.h"

#include <QComboBox>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QVBoxLayout>

namespace fr {

TimelinePanel::TimelinePanel(Session& session, QWidget* parent) : QWidget(parent), session_(session) {
    auto* layout = new QVBoxLayout(this);

    timelines_ = new QComboBox(this);
    timelines_->setObjectName(QStringLiteral("timeline.combo"));
    layout->addWidget(timelines_);

    clips_ = new QListWidget(this);
    clips_->setObjectName(QStringLiteral("timeline.clips"));
    clips_->setToolTip(tr("Drag sources here from the Project list"));
    clips_->viewport()->setAcceptDrops(true);
    clips_->viewport()->installEventFilter(this);
    layout->addWidget(clips_, 1);

    const auto makeButton = [this](const char* id, const QString& text) {
        auto* button = new QPushButton(text, this);
        button->setObjectName(QStringLiteral("timeline.") + QLatin1String(id));
        return button;
    };
    newButton_ = makeButton("new", tr("New"));
    defaultButton_ = makeButton("default", tr("Set Default"));
    addButton_ = makeButton("add", tr("Add Source"));
    removeButton_ = makeButton("remove", tr("Remove"));
    upButton_ = makeButton("up", tr("Move Up"));
    downButton_ = makeButton("down", tr("Move Down"));
    showButton_ = makeButton("show", tr("Play Timeline"));
    newButton_->setToolTip(tr("Start a new, empty timeline"));
    removeButton_->setToolTip(tr("Remove the selected clip from the timeline"));
    defaultButton_->setToolTip(tr("The default timeline is the one sources are added to from the Project list"));
    addButton_->setToolTip(tr("Add the source that is on screen after the selected clip"));
    showButton_->setToolTip(tr("Show the clips back to back in the active viewer"));

    // Two columns, so the panel stays narrow.
    auto* grid = new QGridLayout;
    grid->setSpacing(4);
    grid->addWidget(showButton_, 0, 0, 1, 2);
    grid->addWidget(addButton_, 1, 0);
    grid->addWidget(removeButton_, 1, 1);
    grid->addWidget(upButton_, 2, 0);
    grid->addWidget(downButton_, 2, 1);
    grid->addWidget(newButton_, 3, 0);
    grid->addWidget(defaultButton_, 3, 1);
    layout->addLayout(grid);

    connect(timelines_, &QComboBox::currentIndexChanged, this, [this] {
        if (!refreshing_) refresh();
    });
    connect(clips_, &QListWidget::currentRowChanged, this, [this] { updateButtons(); });

    connect(newButton_, &QPushButton::clicked, this, [this] {
        const QString id =
            session_.addTimeline(tr("Timeline %1").arg(session_.project().timelines.size() + 1));
        const int index = timelines_->findData(id);
        if (index >= 0) timelines_->setCurrentIndex(index);
    });
    connect(defaultButton_, &QPushButton::clicked, this, [this] {
        if (!currentTimelineId().isEmpty()) session_.setDefaultTimeline(currentTimelineId());
    });
    connect(addButton_, &QPushButton::clicked, this, [this] {
        const QString sourceId = session_.activeSourceId();
        if (sourceId.isEmpty() || currentTimelineId().isEmpty()) return;
        const int row = clips_->currentRow();
        session_.addClipToTimeline(currentTimelineId(), sourceId, row >= 0 ? row + 1 : -1);
    });
    connect(removeButton_, &QPushButton::clicked, this, [this] {
        const int row = clips_->currentRow();
        if (row >= 0) session_.removeClipFromTimeline(currentTimelineId(), row);
    });
    connect(upButton_, &QPushButton::clicked, this, [this] {
        const int row = clips_->currentRow();
        if (row <= 0) return;
        session_.moveClipInTimeline(currentTimelineId(), row, row - 1);
        clips_->setCurrentRow(row - 1);
    });
    connect(downButton_, &QPushButton::clicked, this, [this] {
        const int row = clips_->currentRow();
        if (row < 0 || row + 1 >= clips_->count()) return;
        session_.moveClipInTimeline(currentTimelineId(), row, row + 1);
        clips_->setCurrentRow(row + 1);
    });
    connect(showButton_, &QPushButton::clicked, this, [this] {
        QString error;
        if (!session_.activateTimeline(currentTimelineId(), &error)) {
            QMessageBox::warning(this, tr("Cannot show the timeline"), error);
        }
    });

    connect(&session_, &Session::projectReplaced, this, [this] { refresh(); });
    connect(&session_, &Session::projectChanged, this, [this] { refresh(); });
    connect(&session_, &Session::timelinesChanged, this, [this] { refresh(); });
    connect(&session_, &Session::activeSourceChanged, this, [this] { updateButtons(); });

    refresh();
}

QString TimelinePanel::currentTimelineId() const {
    return timelines_->currentData().toString();
}

void TimelinePanel::refresh() {
    refreshing_ = true;
    const QString wanted = currentTimelineId();
    timelines_->clear();
    for (const Timeline& timeline : session_.project().timelines) {
        timelines_->addItem(timeline.isDefault ? tr("%1 (default)").arg(timeline.name) : timeline.name,
                            timeline.id);
    }
    const int index = timelines_->findData(wanted);
    timelines_->setCurrentIndex(index >= 0 ? index : (timelines_->count() > 0 ? 0 : -1));
    refreshing_ = false;

    const int keep = clips_->currentRow();
    clips_->clear();
    if (const Timeline* timeline = session_.project().findTimeline(currentTimelineId())) {
        int number = 1;
        for (const Clip& clip : timeline->clips) {
            const Source* source = session_.project().findSource(clip.sourceId);
            QString text = QStringLiteral("%1. %2").arg(number++).arg(source ? source->name : tr("(missing source)"));
            if (source && (source->cutInFrame > 0 || source->cutOutFrame >= 0)) {
                text += tr("  [cut %1–%2]")
                            .arg(source->cutInFrame + 1)
                            .arg(source->cutOutFrame >= 0 ? QString::number(source->cutOutFrame + 1) : tr("end"));
            }
            clips_->addItem(text);
        }
    }
    if (keep >= 0 && keep < clips_->count()) clips_->setCurrentRow(keep);
    updateButtons();
}

int TimelinePanel::dropRow(const QPoint& position) const {
    const QListWidgetItem* item = clips_->itemAt(position);
    if (!item) return -1;
    const int row = clips_->row(item);
    return position.y() > clips_->visualItemRect(item).center().y() ? row + 1 : row;
}

// Sources dragged from the Project list are inserted where they are dropped. With no timeline yet,
// the drop starts one.
bool TimelinePanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched != clips_->viewport()) return QWidget::eventFilter(watched, event);
    const QLatin1String mime(kSourceIdsMime);
    switch (event->type()) {
    case QEvent::DragEnter:
    case QEvent::DragMove: {
        auto* drag = static_cast<QDragMoveEvent*>(event);
        if (drag->mimeData()->hasFormat(mime)) {
            drag->setDropAction(Qt::CopyAction);  // never Move: the Project list would drop its item
            drag->accept();
        } else {
            drag->ignore();
        }
        return true;
    }
    case QEvent::Drop: {
        auto* drop = static_cast<QDropEvent*>(event);
        if (!drop->mimeData()->hasFormat(mime)) {
            drop->ignore();
            return true;
        }
        const QStringList ids =
            QString::fromUtf8(drop->mimeData()->data(mime)).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        int row = dropRow(drop->position().toPoint());
        QString timelineId = currentTimelineId();
        if (timelineId.isEmpty()) {
            timelineId = session_.addTimeline(tr("Timeline %1").arg(session_.project().timelines.size() + 1));
            const int index = timelines_->findData(timelineId);
            if (index >= 0) timelines_->setCurrentIndex(index);
        }
        int last = -1;
        for (const QString& id : ids) {
            if (!session_.project().findSource(id)) continue;
            session_.addClipToTimeline(timelineId, id, row);
            last = row >= 0 ? row++ : clips_->count() - 1;
        }
        if (last >= 0) clips_->setCurrentRow(last);
        drop->setDropAction(Qt::CopyAction);
        drop->accept();
        return true;
    }
    default:
        return QWidget::eventFilter(watched, event);
    }
}

void TimelinePanel::updateButtons() {
    const bool hasTimeline = !currentTimelineId().isEmpty();
    const int row = clips_->currentRow();
    defaultButton_->setEnabled(hasTimeline);
    showButton_->setEnabled(hasTimeline && clips_->count() > 0);
    addButton_->setEnabled(hasTimeline && !session_.activeSourceId().isEmpty());
    removeButton_->setEnabled(row >= 0);
    upButton_->setEnabled(row > 0);
    downButton_->setEnabled(row >= 0 && row + 1 < clips_->count());
}

}  // namespace fr
