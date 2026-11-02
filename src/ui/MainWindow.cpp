#include "ui/MainWindow.h"

#include "api/ApiServer.h"
#include "core/AudioController.h"
#include "core/Exporter.h"
#include "media/ImageSequence.h"
#include "ui/BookmarkSlider.h"
#include "ui/ExportDialog.h"
#include "ui/PreferencesDialog.h"
#include "ui/TimelinePanel.h"
#include "ui/ViewerArea.h"
#include "ui/ViewerWidget.h"

#include <QActionGroup>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QTreeWidgetItemIterator>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QKeyEvent>
#include <QPainter>
#include <QKeySequenceEdit>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QProgressDialog>
#include <QSet>
#include <QSignalBlocker>
#include <QSlider>
#include <QStatusBar>
#include <QStyle>
#include <QTableWidget>
#include <QToolBar>
#include <QToolButton>
#include <QTimer>
#include <QTreeWidget>
#include <QUndoStack>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <iterator>
#include <cmath>
#include <optional>

namespace fr {
namespace {

enum ItemKind { KindFolder = 0, KindSource = 1, KindTimeline = 2 };
constexpr int kIdRole = Qt::UserRole;
constexpr int kKindRole = Qt::UserRole + 1;

// The Project list: sources can be dragged onto the Timeline panel (copied, never moved).
class ProjectTree : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;

protected:
    QStringList mimeTypes() const override { return {QString::fromLatin1(TimelinePanel::kSourceIdsMime)}; }
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override {
        // Several selected sources go in the order the list shows them, whatever order they were picked in.
        const QSet<const QTreeWidgetItem*> picked(items.cbegin(), items.cend());
        QStringList ids;
        for (QTreeWidgetItemIterator it(const_cast<ProjectTree*>(this)); *it; ++it) {
            if (picked.contains(*it) && (*it)->data(0, kKindRole).toInt() == KindSource) {
                ids << (*it)->data(0, kIdRole).toString();
            }
        }
        if (ids.isEmpty()) return nullptr;  // folders and timelines do not drag
        auto* data = new QMimeData;
        data->setData(QString::fromLatin1(TimelinePanel::kSourceIdsMime), ids.join(QLatin1Char('\n')).toUtf8());
        return data;
    }
    Qt::DropActions supportedDropActions() const override { return Qt::CopyAction; }
};

QString mediaFilter() {
    return QObject::tr("Media (*.mp4 *.mov *.mkv *.avi *.webm *.mxf *.mpg *.mpeg *.m4v *.flv *.wmv *.mts "
                       "*.png *.jpg *.jpeg *.bmp *.gif *.tif *.tiff *.webp);;All files (*)");
}

// "4.0 s", "1 min 23 s", "1 h 02 min": the length of a clip in words, next to the exact clock time.
QString spokenDuration(int frames, double fps) {
    const double seconds = std::max(0, frames) / (fps > 0.0 ? fps : 24.0);
    if (seconds < 60.0) return QObject::tr("%1 s").arg(seconds, 0, 'f', 1);
    const qint64 whole = std::llround(seconds);
    if (whole < 3600) return QObject::tr("%1 min %2 s").arg(whole / 60).arg(whole % 60);
    return QObject::tr("%1 h %2 min").arg(whole / 3600).arg((whole / 60) % 60, 2, 10, QLatin1Char('0'));
}

// "1 frame", "96 frames": the app ships without translation files, so "%n frame(s)" would show as is.
QString countOf(int n, const QString& one, const QString& many) {
    return QStringLiteral("%1 %2").arg(n).arg(n == 1 ? one : many);
}

// A file name that may wrap after its separators: a narrow Info panel cannot break "shot.[0001-0120].png"
// otherwise. The marks are zero-width, so nothing changes on screen.
QString breakable(QString text) {
    static const QRegularExpression separators(QStringLiteral("([._\\-\\[/])"));
    return text.replace(separators, QStringLiteral("\\1\u200B"));
}

QString fpsText(double fps) {
    return QObject::tr("%1 fps").arg(QString::number(fps, 'f', 3).remove(QRegularExpression(QStringLiteral("\\.?0+$"))));
}

// A small grey caption over a value, for the readouts under the viewer.
QWidget* readout(const QString& caption, QWidget* value, QLabel** captionLabel = nullptr) {
    auto* box = new QWidget;
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(4, 0, 4, 0);
    layout->setSpacing(0);
    auto* label = new QLabel(caption);
    QFont small = label->font();
    small.setPointSizeF(small.pointSizeF() * 0.78);
    small.setBold(true);
    label->setFont(small);
    label->setStyleSheet(QStringLiteral("color: #8c8c90;"));
    layout->addWidget(label);
    layout->addWidget(value);
    if (captionLabel) *captionLabel = label;
    return box;
}

// A small drawing of a viewer layout, for its menu entry and toolbar button.
QIcon layoutIcon(const QString& layout) {
    QPixmap pixmap(32, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor a(74, 163, 255);
    const QColor b(255, 166, 74);
    const QRectF frame(2.5, 3.5, 27, 17);
    const auto letter = [&painter](const QRectF& box, const QString& text) {
        QFont font = painter.font();
        font.setPixelSize(9);
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(Qt::white);
        painter.drawText(box, Qt::AlignCenter, text);
    };
    if (layout == QLatin1String("horizontal") || layout == QLatin1String("split_horizontal")) {
        const double gap = layout == QLatin1String("horizontal") ? 1.5 : 0.0;
        const QRectF left(frame.left(), frame.top(), frame.width() / 2 - gap, frame.height());
        const QRectF right(frame.center().x() + gap, frame.top(), frame.width() / 2 - gap, frame.height());
        painter.fillRect(left, a);
        painter.fillRect(right, b);
        letter(left, QStringLiteral("A"));
        letter(right, QStringLiteral("B"));
    } else if (layout == QLatin1String("vertical") || layout == QLatin1String("split_vertical")) {
        const double gap = layout == QLatin1String("vertical") ? 1.5 : 0.0;
        const QRectF top(frame.left(), frame.top(), frame.width(), frame.height() / 2 - gap);
        const QRectF bottom(frame.left(), frame.center().y() + gap, frame.width(), frame.height() / 2 - gap);
        painter.fillRect(top, a);
        painter.fillRect(bottom, b);
        letter(top, QStringLiteral("A"));
        letter(bottom, QStringLiteral("B"));
    } else {
        painter.fillRect(frame, a);
        letter(frame, QStringLiteral("A"));
    }
    if (layout.startsWith(QLatin1String("split_"))) {  // one frame with a wipe line
        painter.setPen(QPen(Qt::white, 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(frame);
    }
    painter.end();
    return QIcon(pixmap);
}

QFrame* separatorLine() {
    auto* line = new QFrame;
    line->setFrameShape(QFrame::VLine);
    line->setStyleSheet(QStringLiteral("color: #444;"));
    return line;
}

}  // namespace

MainWindow::MainWindow(Session& session, QWidget* parent) : QMainWindow(parent), session_(session) {
    // 1440 × 900, or most of a smaller screen.
    QSize size(1440, 900);
    if (const QScreen* screen = QGuiApplication::primaryScreen()) {
        const QSize available = screen->availableGeometry().size();
        size = size.boundedTo(QSize(available.width() * 95 / 100, available.height() * 92 / 100));
    }
    resize(size);
    setAcceptDrops(true);
    area_ = new ViewerArea(this);

    session_.setCacheMegabytes(prefs_.cacheMegabytes());
    session_.project().imageSeqFrameRate = prefs_.sequenceFrameRate();
    numbering_ = static_cast<NumberingMode>(prefs_.numberingMode());
    audio_ = new AudioController(session_, this);
    audio_->setMuted(prefs_.audioMuted());
    audio_->setVolume(prefs_.audioVolume());
    audio_->setScrubEnabled(prefs_.audioScrub());
    audio_->setBufferMs(prefs_.audioBufferMs());

    buildDocks();
    buildMenus();
    buildTransport();
    buildTools();
    buildBookmarks();
    buildHelpMenu();
    for (const Command& command : commands()) defaultShortcuts_.insert(command.id, command.action->shortcut());
    applyShortcutOverrides();
    arrangeDocks();
    restoreLayout();
    if (prefs_.alwaysOnTop()) setWindowFlag(Qt::WindowStaysOnTopHint, true);

    // The viewer layouts, one click each: A alone, A and B side by side or stacked, or split.
    auto* layouts = new QWidget(this);
    auto* layoutRow = new QHBoxLayout(layouts);
    layoutRow->setContentsMargins(0, 0, 12, 0);
    layoutRow->setSpacing(0);
    auto* layoutCaption = new QLabel(tr("Viewer"), layouts);
    layoutCaption->setStyleSheet(QStringLiteral("color: #8c8c90; padding-right: 4px;"));
    layoutRow->addWidget(layoutCaption);
    for (QAction* action : layoutMenu_->actions()) {
        auto* button = new QToolButton(layouts);
        button->setDefaultAction(action);
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setIconSize(QSize(28, 20));
        button->setAutoRaise(true);
        layoutRow->addWidget(button);
    }
    statusBar()->addPermanentWidget(layouts);

    statusInfo_ = new QLabel(this);
    statusInfo_->setObjectName(QStringLiteral("status.info"));
    statusBar()->addPermanentWidget(statusInfo_);

    PlaybackController& playback = session_.playback();
    connect(&playback, &PlaybackController::frameChanged, this, &MainWindow::onFrameChanged);
    connect(&playback, &PlaybackController::configured, this, &MainWindow::onConfigured);
    connect(&playback, &PlaybackController::rangeChanged, this, [this](int start, int end) {
        const QSignalBlocker blocker(rangeSlider_);
        rangeSlider_->setRange(start, end);
        rangeSlider_->setEnabled(end > start);
        rangeSlider_->setValue(session_.playback().frame());
        refreshFrameLabel();
        refreshInfo();
    });
    connect(&playback, &PlaybackController::playingChanged, this, [this](bool playing) {
        playAction_->setText(playing ? tr("Pause") : tr("Play"));
        playAction_->setIcon(style()->standardIcon(playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
        playAction_->setProperty("kenaeproBaseToolTip", playAction_->text());
        // While the picture moves a note would land on whichever frame is passing: read only.
        noteEdit_->setReadOnly(playing);
        addShortcutTooltips();
    });

    connect(&session_, &Session::projectReplaced, this, [this] {
        dirty_ = false;
        lastSourceId_.clear();
        refreshTree();
        refreshViews();
        refreshNote();
        refreshBookmarks();
        refreshFrameLabel();
        refreshInfo();
        updateTitle();
    });
    connect(&session_, &Session::projectChanged, this, [this] {
        dirty_ = true;
        refreshTree();
        refreshFrameLabel();
        refreshInfo();
        updateTitle();
    });
    connect(&session_, &Session::activeSourceChanged, this, [this] {
        lastSourceId_ = session_.activeSourceId();
        refreshTree();
        refreshViews();
        refreshNote();
        refreshBookmarks();
        refreshFrameLabel();
        refreshInfo();
        for (int i = 0; i < 2; ++i) applyViewState(i);
        updateTitle();
    });
    connect(&session_, &Session::activeViewerChanged, this, [this](int viewer) {
        area_->setActiveViewer(viewer);
        lastSourceId_ = session_.activeSourceId();
        refreshTree();
        refreshNote();
        refreshBookmarks();
        refreshFrameLabel();
        refreshInfo();
    });
    connect(&session_, &Session::viewerLayoutChanged, this, [this] {
        area_->setLayoutMode(session_.viewerLayout());
        area_->setActiveViewer(session_.activeViewer());
        if (layoutMenu_) {
            for (QAction* action : layoutMenu_->actions()) {
                action->setChecked(action->data().toString() == session_.viewerLayout());
            }
        }
        refreshViews();
        refreshTree();
    });
    connect(&session_, &Session::viewerOffsetChanged, this, [this] {
        refreshViews();
        refreshNote();
        refreshBookmarks();
        refreshFrameLabel();
        refreshInfo();
    });
    connect(&session_, &Session::annotationsChanged, this, [this] {
        dirty_ = true;
        refreshViews();
        refreshInfo();
        if (session_.bookmarkFilter() != BookmarkFilter::All) refreshBookmarks();
        updateTitle();
    });
    connect(&session_, &Session::frameImageReady, this, [this](int viewer) { refreshViewer(viewer); });
    connect(&session_, &Session::bookmarksChanged, this, [this] {
        dirty_ = true;
        refreshBookmarks();
        refreshInfo();
        updateTitle();
    });

    for (int i = 0; i < 2; ++i) {
        ViewerWidget* viewer = area_->viewer(i);
        connect(viewer, &ViewerWidget::strokeFinished, this,
                [this, i](const Stroke& stroke) { onStrokeFinished(sessionViewerFor(i), stroke); });
        connect(viewer, &ViewerWidget::textRequested, this,
                [this, i](const QPointF& at) { onTextRequested(sessionViewerFor(i), at); });
        connect(viewer, &ViewerWidget::eraseFinished, this, [this, i, viewer](const QVector<QPointF>& path) {
            const int target = sessionViewerFor(i);
            session_.setActiveViewer(target);
            session_.erasePath(path, 0.03, viewer->imageAspect(viewer->pressedSide()));
        });
        connect(viewer, &ViewerWidget::strokeMoved, this, [this, i](int index, const QPointF& delta) {
            session_.setActiveViewer(sessionViewerFor(i));
            session_.moveStroke(index, delta);
        });
        connect(viewer, &ViewerWidget::colorPicked, this, [this](const QColor& color) { onColorPicked(color); });
        // Drawings belong to one frame: hold the picture still from the moment the pen goes down.
        connect(viewer, &ViewerWidget::gestureStarted, this, [this] { session_.playback().pause(); });
        connect(viewer, &ViewerWidget::viewChanged, this, [this, i] { storeView(i); });
        connect(viewer, &ViewerWidget::scrubbed, this, [this](int frames) {
            PlaybackController& playback = session_.playback();
            playback.pause();
            playback.seek(playback.frame() + frames);
        });
        viewer->setScrubPixelsPerFrame(prefs_.scrubPixelsPerFrame());
    }
    connect(area_, &ViewerArea::viewerActivated, this,
            [this](int viewer) { session_.setActiveViewer(sessionViewerFor(viewer)); });

    area_->setLayoutMode(session_.viewerLayout());
    updateRecentMenu();
    refreshTree();
    refreshViews();
    refreshNote();
    refreshBookmarks();
    refreshFrameLabel();
    refreshInfo();
    updateTitle();
    statusBar()->showMessage(tr("Ready. Press F1 for the keyboard shortcuts."), 8000);
}

MainWindow::~MainWindow() = default;

// --- building --------------------------------------------------------------------------------

void MainWindow::buildDocks() {
    buildInfo();

    tree_ = new ProjectTree(this);
    tree_->setObjectName(QStringLiteral("project.tree"));
    tree_->setDragEnabled(true);
    tree_->setDragDropMode(QAbstractItemView::DragOnly);
    tree_->setDefaultDropAction(Qt::CopyAction);
    tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);  // Shift: a run, Ctrl: one more
    tree_->setHeaderHidden(true);
    tree_->setColumnCount(2);  // the name, and "A" / "B" for what the viewers show
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item, int) { onTreeActivated(item); });
    connect(tree_, &QWidget::customContextMenuRequested, this, &MainWindow::onTreeContextMenu);
    auto* projectDock = new QDockWidget(tr("Project"), this);
    projectDock->setObjectName(QStringLiteral("dock.project"));
    projectDock->setWidget(tree_);
    addDockWidget(Qt::LeftDockWidgetArea, projectDock);

    timelinePanel_ = new TimelinePanel(session_, this);
    auto* timelineDock = new QDockWidget(tr("Timeline"), this);
    timelineDock->setObjectName(QStringLiteral("dock.timeline"));
    timelineDock->setWidget(timelinePanel_);
    addDockWidget(Qt::LeftDockWidgetArea, timelineDock);

    noteEdit_ = new QPlainTextEdit(this);
    noteEdit_->setPlaceholderText(tr("Click here to type a note for the current frame. Esc (or a click on "
                                     "the picture) gives the keys back to playback. Frames with a note "
                                     "get a green mark on the slider."));
    noteEdit_->installEventFilter(this);
    noteEdit_->setAcceptDrops(false);  // a dropped file goes to the window (import), not into the note
    connect(noteEdit_, &QPlainTextEdit::textChanged, this, &MainWindow::onNoteEdited);
    auto* notesDock = new QDockWidget(tr("Notes"), this);
    notesDock->setObjectName(QStringLiteral("dock.notes"));
    notesDock->setWidget(noteEdit_);
    addDockWidget(Qt::RightDockWidgetArea, notesDock);
}

void MainWindow::buildMenus() {
    buildFileMenu();
    buildEditMenu();
    buildViewMenu();
    buildSourceMenu();
    buildPlaybackMenu();
    buildAudioMenu();
}

void MainWindow::buildFileMenu() {
    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&New Project"), QKeySequence::New, this, [this] {
        if (!maybeSave()) return;
        session_.newProject();
        session_.project().imageSeqFrameRate = prefs_.sequenceFrameRate();
    });
    file->addAction(tr("&Open Project…"), QKeySequence::Open, this, &MainWindow::openProject);
    recentMenu_ = file->addMenu(tr("Open &Recent"));
    file->addAction(tr("&Save"), QKeySequence::Save, this, &MainWindow::saveProject);
    file->addAction(tr("Save &As…"), QKeySequence::SaveAs, this, &MainWindow::saveProjectAs);
    file->addSeparator();
    file->addAction(tr("&Import Media…"), QKeySequence(Qt::CTRL | Qt::Key_I), this, &MainWindow::importMedia);
    file->addAction(tr("Import Image &Sequence…"), QKeySequence(), this, &MainWindow::importSequenceDialog);
    file->addAction(tr("Import &Folder…"), QKeySequence(), this, &MainWindow::importFolderDialog);
    file->addSeparator();
    file->addAction(tr("&Export…"), QKeySequence(Qt::CTRL | Qt::Key_E), this, &MainWindow::exportMedia);
    file->addAction(tr("Save Frame as &Image…"), QKeySequence(), this, &MainWindow::saveFrameImage);
    file->addAction(tr("&Copy Frame"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C), this, &MainWindow::copyFrame);
    file->addSeparator();
    file->addAction(tr("&Preferences…"), QKeySequence(Qt::CTRL | Qt::Key_Comma), this, &MainWindow::showPreferences);
    file->addAction(tr("&Quit"), QKeySequence::Quit, this, &QWidget::close);
}

void MainWindow::buildEditMenu() {
    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    QAction* undo = session_.undoStack().createUndoAction(this, tr("Undo"));
    undo->setObjectName(QStringLiteral("action.undo"));
    undo->setShortcut(QKeySequence::Undo);
    QAction* redo = session_.undoStack().createRedoAction(this, tr("Redo"));
    redo->setObjectName(QStringLiteral("action.redo"));
    redo->setShortcut(QKeySequence::Redo);
    edit->addAction(undo);
    edit->addAction(redo);
    edit->addSeparator();

    addMenuAction(edit, "clearDrawings", tr("Clear Drawings on This Frame"), QKeySequence(),
                  [this] { session_.clearStrokes(); });
    addMenuAction(edit, "deleteDrawing", tr("Delete Selected Drawing"), QKeySequence(),
                  [this] { session_.deleteStroke(widgetFor(session_.activeViewer())->selectedStroke()); });
    edit->addSeparator();
    addMenuAction(edit, "copyDrawings", tr("Copy Drawings"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_C),
                  [this] { session_.copyStrokes(); });
    addMenuAction(edit, "cutDrawings", tr("Cut Drawings"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_X),
                  [this] { session_.cutStrokes(); });
    addMenuAction(edit, "pasteDrawings", tr("Paste Drawings"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_V),
                  [this] { session_.pasteStrokes(); });
}

void MainWindow::buildViewMenu() {
    QMenu* view = menuBar()->addMenu(tr("&View"));

    QMenu* overlay = view->addMenu(tr("Overlay"));
    auto* overlayGroup = new QActionGroup(this);
    const struct {
        QString label;
        ViewerWidget::Overlay value;
    } overlays[] = {
        {tr("None"), ViewerWidget::Overlay::None},
        {tr("Center"), ViewerWidget::Overlay::Center},
        {tr("Thirds grid"), ViewerWidget::Overlay::Thirds},
        {tr("Phi grid"), ViewerWidget::Overlay::Phi},
        {tr("Dynamic symmetry"), ViewerWidget::Overlay::DynamicSymmetry},
        {tr("Safe area"), ViewerWidget::Overlay::SafeArea},
    };
    for (const auto& entry : overlays) {
        QAction* action = overlay->addAction(entry.label);
        action->setCheckable(true);
        action->setActionGroup(overlayGroup);
        action->setChecked(entry.value == ViewerWidget::Overlay::None);
        connect(action, &QAction::triggered, this, [this, value = entry.value] {
            for (int i = 0; i < 2; ++i) area_->viewer(i)->setOverlay(value);
        });
    }

    QAction* flip = addMenuAction(view, "flipHorizontal", tr("Flip Horizontal"), QKeySequence(Qt::SHIFT | Qt::Key_F),
                                  [this](bool checked) {
                                      for (int i = 0; i < 2; ++i) area_->viewer(i)->setFlipped(checked);
                                  });
    flip->setCheckable(true);
    QAction* flipV = addMenuAction(view, "flipVertical", tr("Flip Vertical"), QKeySequence(Qt::SHIFT | Qt::Key_V),
                                   [this](bool checked) {
                                       for (int i = 0; i < 2; ++i) area_->viewer(i)->setFlippedVertically(checked);
                                   });
    flipV->setCheckable(true);
    addMenuAction(view, "rotateClockwise", tr("Rotate 90° Clockwise"), QKeySequence(Qt::CTRL | Qt::Key_R),
                  [this] { rotateViewers(1); });
    addMenuAction(view, "rotateCounterClockwise", tr("Rotate 90° Counter-clockwise"),
                  QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R), [this] { rotateViewers(-1); });
    addMenuAction(view, "resetRotation", tr("Reset Rotation"), QKeySequence(),
                  [this] { rotateViewers(-area_->viewer(0)->rotation()); });
    addMenuAction(view, "cycleBackground", tr("Cycle Viewer Background"), QKeySequence(Qt::Key_K),
                  [this] { for (int i = 0; i < 2; ++i) area_->viewer(i)->cycleBackground(); });
    addMenuAction(view, "resetZoom", tr("Reset Zoom"), QKeySequence(Qt::Key_0),
                  [this] { for (int i = 0; i < 2; ++i) area_->viewer(i)->resetView(); });
    view->addSeparator();

    layoutMenu_ = view->addMenu(tr("Layout"));
    auto* layoutGroup = new QActionGroup(this);
    const struct {
        QString label;
        QString value;
        Qt::Key key;
    } layouts[] = {
        {tr("One Viewer (A)"), QStringLiteral("single"), Qt::Key_1},
        {tr("A and B Side by Side"), QStringLiteral("horizontal"), Qt::Key_2},
        {tr("A and B Stacked"), QStringLiteral("vertical"), Qt::Key_3},
        {tr("Split A | B (Wipe Left/Right)"), QStringLiteral("split_horizontal"), Qt::Key_4},
        {tr("Split A / B (Wipe Top/Bottom)"), QStringLiteral("split_vertical"), Qt::Key_5},
    };
    for (const auto& entry : layouts) {
        QAction* action = layoutMenu_->addAction(entry.label);
        action->setIcon(layoutIcon(entry.value));
        action->setShortcut(QKeySequence(Qt::ALT | entry.key));
        action->setCheckable(true);
        action->setActionGroup(layoutGroup);
        action->setData(entry.value);
        action->setObjectName(QStringLiteral("layout.") + entry.value);
        action->setChecked(entry.value == session_.viewerLayout());
        connect(action, &QAction::triggered, this, [this, value = entry.value] { session_.setViewerLayout(value); });
    }
    addMenuAction(view, "swapAB", tr("Swap A and B"), QKeySequence(Qt::ALT | Qt::Key_0), [this] {
        const QString a = session_.viewerTargetId(0);
        const QString b = session_.viewerTargetId(1);
        if (session_.viewerCount() < 2 || a.isEmpty() || b.isEmpty()) return;
        const int active = session_.activeViewer();
        for (const auto& [viewer, id] : {std::pair{0, b}, std::pair{1, a}}) {
            session_.setActiveViewer(viewer);
            QString error;
            const bool ok = session_.project().findTimeline(id) ? session_.activateTimeline(id, &error)
                                                                : session_.activateSource(id, &error);
            if (!ok) showError(tr("Cannot swap"), error);
        }
        session_.setActiveViewer(active);
    });
    addMenuAction(view, "viewerOffset", tr("Second Viewer Offset…"), QKeySequence(), [this] {
        bool ok = false;
        const int value = QInputDialog::getInt(this, tr("Second Viewer Offset"),
                                               tr("Frames the second viewer is shifted against the first:"),
                                               session_.viewerOffset(1), -100000, 100000, 1, &ok);
        if (ok) session_.setViewerOffset(1, value);
    });
    view->addSeparator();

    numberingMenu_ = view->addMenu(tr("Frame Numbering"));
    auto* numberingGroup = new QActionGroup(this);
    const struct {
        QString label;
        NumberingMode value;
    } modes[] = {
        {tr("Global"), NumberingMode::Global},
        {tr("Image file number"), NumberingMode::Image},
        {tr("Inside the range"), NumberingMode::Range},
        {tr("Timecode"), NumberingMode::Timecode},
    };
    for (const auto& entry : modes) {
        QAction* action = numberingMenu_->addAction(entry.label);
        action->setCheckable(true);
        action->setActionGroup(numberingGroup);
        action->setChecked(entry.value == numbering_);
        connect(action, &QAction::triggered, this, [this, value = entry.value] {
            numbering_ = value;
            prefs_.setNumberingMode(static_cast<int>(value));
            refreshFrameLabel();
        });
    }

    QMenu* ghosting = view->addMenu(tr("Ghosting"));
    auto* ghostGroup = new QActionGroup(this);
    for (const int frames : {0, 1, 2, 3}) {
        QAction* action = ghosting->addAction(frames == 0 ? tr("Off") : tr("%1 each side").arg(countOf(frames, tr("frame"), tr("frames"))));
        action->setCheckable(true);
        action->setActionGroup(ghostGroup);
        action->setChecked(frames == 0);
        connect(action, &QAction::triggered, this, [this, frames] {
            ghostFrames_ = frames;
            refreshViews();
        });
    }
    ghosting->addSeparator();
    QAction* ghostDrawings = ghosting->addAction(tr("Drawings of the neighbouring annotated frames"));
    ghostDrawings->setCheckable(true);
    connect(ghostDrawings, &QAction::triggered, this, [this](bool checked) {
        ghostDrawings_ = checked;
        refreshViews();
    });

    view->addSeparator();
    // Shown or hidden once the transport bar exists; both start shown.
    QAction* global = addMenuAction(view, "showGlobalTimeline", tr("Global Timeline"), QKeySequence(),
                                    [this](bool checked) { if (globalRow_) globalRow_->setVisible(checked); });
    global->setCheckable(true);
    global->setChecked(true);
    QAction* range = addMenuAction(view, "showRangeTimeline", tr("Range Timeline"), QKeySequence(),
                                   [this](bool checked) { if (rangeRow_) rangeRow_->setVisible(checked); });
    range->setCheckable(true);
    range->setChecked(true);

    view->addSeparator();
    // Filled when the menu opens: the Bookmarks dock is created after the menus.
    QMenu* panels = view->addMenu(tr("Panels"));
    connect(panels, &QMenu::aboutToShow, this, [this, panels] {
        panels->clear();
        for (QDockWidget* dock : findChildren<QDockWidget*>()) panels->addAction(dock->toggleViewAction());
    });
    addMenuAction(view, "resetLayout", tr("Reset Panel Layout"), QKeySequence(), [this] { resetLayout(); });
    QAction* onTop = addMenuAction(view, "alwaysOnTop", tr("Always on Top"), QKeySequence(Qt::CTRL | Qt::Key_T),
                                   [this](bool checked) { setAlwaysOnTop(checked); });
    onTop->setCheckable(true);
    onTop->setChecked(prefs_.alwaysOnTop());
}

void MainWindow::buildSourceMenu() {
    sourceMenu_ = menuBar()->addMenu(tr("&Source"));
    QMenu* menu = sourceMenu_;
    addMenuAction(menu, "setCutIn", tr("Set Cut In Here"), QKeySequence(), [this] { session_.setCutInHere(); });
    addMenuAction(menu, "setCutOut", tr("Set Cut Out Here"), QKeySequence(), [this] { session_.setCutOutHere(); });
    addMenuAction(menu, "clearCut", tr("Clear Cut"), QKeySequence(), [this] { session_.clearCut(); });
    menu->addSeparator();

    addMenuAction(menu, "tcOverride", tr("TC Frame Offset Override…"), QKeySequence(), [this] {
        const Source* source = session_.locationSource();
        if (!source) return;
        bool ok = false;
        const int value = QInputDialog::getInt(this, tr("TC Frame Offset Override"),
                                               tr("Frames added to this source's frame number and timecode:"),
                                               source->tcOffsetOverride, -10000000, 10000000, 1, &ok);
        if (ok) session_.setTcOffsetOverride(true, value);
    });
    addMenuAction(menu, "clearTcOverride", tr("Clear TC Frame Offset Override"), QKeySequence(),
                  [this] { session_.setTcOffsetOverride(false, 0); });
    addMenuAction(menu, "projectTcOffset", tr("Project TC Frame Offset…"), QKeySequence(), [this] {
        bool ok = false;
        const int value = QInputDialog::getInt(this, tr("Project TC Frame Offset"),
                                               tr("Frames added to every frame number and timecode:"),
                                               session_.project().timeControlFrameOffset, -10000000, 10000000, 1, &ok);
        if (ok) session_.setProjectTcOffset(value);
    });
    addMenuAction(menu, "audioDelay", tr("Audio Delay…"), QKeySequence(), [this] {
        const Source* source = session_.locationSource();
        if (!source) return;
        bool ok = false;
        const double value = QInputDialog::getDouble(this, tr("Audio Delay"),
                                                     tr("Milliseconds the sound is delayed against the picture:"),
                                                     source->audioDelayMs, -60000.0, 60000.0, 1, &ok);
        if (ok) session_.setAudioDelayMs(value);
    });
    menu->addSeparator();

    // These toggle the source's own state, so a key works whatever the menu last showed.
    QAction* hold = addMenuAction(menu, "holdDrawings", tr("Hold Drawings"), QKeySequence(), [this] {
        if (const Source* source = session_.locationSource()) session_.setHoldAnnotations(!source->holdAnnotations);
    });
    hold->setCheckable(true);
    QAction* panZoom = addMenuAction(menu, "rememberView", tr("Remember Zoom and Pan for This Source"), QKeySequence(),
                                     [this] {
                                         const Source* source = session_.locationSource();
                                         if (!source) return;
                                         session_.setPanZoomEnabled(!source->panZoomEnabled);
                                         storeView(session_.activeViewer());
                                     });
    panZoom->setCheckable(true);
    connect(menu, &QMenu::aboutToShow, this, [this, hold, panZoom] {
        const Source* source = session_.locationSource();
        hold->setChecked(source && source->holdAnnotations);
        panZoom->setChecked(source && source->panZoomEnabled);
    });
}

void MainWindow::buildPlaybackMenu() {
    QMenu* menu = menuBar()->addMenu(tr("&Playback"));
    PlaybackController& playback = session_.playback();
    playAction_ = addMenuAction(menu, "play", tr("Play"), QKeySequence(Qt::Key_Space),
                                [&playback] { playback.togglePlay(); });
    playAction_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    addMenuAction(menu, "previousFrame", tr("Previous Frame"), QKeySequence(Qt::Key_Left),
                  [&playback] { playback.step(-1); })
        ->setIcon(style()->standardIcon(QStyle::SP_MediaSeekBackward));
    addMenuAction(menu, "nextFrame", tr("Next Frame"), QKeySequence(Qt::Key_Right),
                  [&playback] { playback.step(1); })
        ->setIcon(style()->standardIcon(QStyle::SP_MediaSeekForward));
    addMenuAction(menu, "backOneSecond", tr("Back One Second"), QKeySequence(Qt::SHIFT | Qt::Key_Left),
                  [this] { stepSeconds(-1); });
    addMenuAction(menu, "forwardOneSecond", tr("Forward One Second"), QKeySequence(Qt::SHIFT | Qt::Key_Right),
                  [this] { stepSeconds(1); });
    steppedAction_ = addMenuAction(menu, "steppedPlayback", tr("Stepped Playback (Bookmarks Only)"),
                                   QKeySequence(Qt::SHIFT | Qt::Key_S),
                                   [this](bool checked) { session_.playback().setStepped(checked); });
    steppedAction_->setCheckable(true);
    steppedAction_->setToolTip(tr("Play only the bookmarked frames, each held until the next one"));
    menu->addSeparator();
    addMenuAction(menu, "previousAnnotated", tr("Previous Frame with Drawings or Notes"),
                  QKeySequence(Qt::ALT | Qt::Key_Left), [this] { jumpToAnnotation(-1); });
    addMenuAction(menu, "nextAnnotated", tr("Next Frame with Drawings or Notes"),
                  QKeySequence(Qt::ALT | Qt::Key_Right), [this] { jumpToAnnotation(1); });
    menu->addSeparator();
    addMenuAction(menu, "faster", tr("Faster"), QKeySequence(Qt::CTRL | Qt::Key_BracketRight),
                  [this] { changeSpeed(2.0); });
    addMenuAction(menu, "slower", tr("Slower"), QKeySequence(Qt::CTRL | Qt::Key_BracketLeft),
                  [this] { changeSpeed(0.5); });
    addMenuAction(menu, "normalSpeed", tr("Normal Speed"), QKeySequence(Qt::CTRL | Qt::Key_1), [this] {
        session_.playback().setSpeed(1.0);
        syncSpeedCombo();
    });
    addMenuAction(menu, "halfSpeed", tr("Half Speed"), QKeySequence(Qt::CTRL | Qt::Key_2), [this] {
        session_.playback().setSpeed(0.5);
        syncSpeedCombo();
    });
    addMenuAction(menu, "customSpeed", tr("Custom Speed…"), QKeySequence(), [this] { customSpeed(); });
    menu->addSeparator();
    addMenuAction(menu, "goToFrame", tr("Go to Frame…"), QKeySequence(Qt::CTRL | Qt::Key_G), [this] { goToFrame(); });
    addMenuAction(menu, "goToStart", tr("Go to Range Start"), QKeySequence(Qt::Key_Home), [this] {
        session_.playback().pause();
        session_.playback().seek(session_.playback().rangeStart());
    })->setIcon(style()->standardIcon(QStyle::SP_MediaSkipBackward));
    addMenuAction(menu, "goToEnd", tr("Go to Range End"), QKeySequence(Qt::Key_End), [this] {
        session_.playback().pause();
        session_.playback().seek(session_.playback().rangeEnd());
    })->setIcon(style()->standardIcon(QStyle::SP_MediaSkipForward));
}

void MainWindow::buildAudioMenu() {
    QMenu* menu = menuBar()->addMenu(tr("&Audio"));
    QAction* mute = addMenuAction(menu, "mute", tr("Mute"), QKeySequence(Qt::Key_M), [this](bool checked) {
        audio_->setMuted(checked);
        prefs_.setAudioMuted(checked);
    });
    mute->setCheckable(true);
    mute->setChecked(audio_->muted());
    QAction* scrub = addMenuAction(menu, "scrubAudio", tr("Scrub Audio"), QKeySequence(), [this](bool checked) {
        audio_->setScrubEnabled(checked);
        prefs_.setAudioScrub(checked);
    });
    scrub->setCheckable(true);
    scrub->setChecked(audio_->scrubEnabled());
    menu->addSeparator();
    const auto nudge = [this](double delta) {
        setVolumePercent(qRound((audio_->volume() + delta) * 100.0));
        statusBar()->showMessage(tr("Volume %1%").arg(qRound(audio_->volume() * 100.0)), 2000);
    };
    // Ctrl+Alt+arrows switch workspaces on many Linux desktops, so plain Ctrl+arrows.
    addMenuAction(menu, "volumeUp", tr("Volume Up"), QKeySequence(Qt::CTRL | Qt::Key_Up),
                  [nudge] { nudge(0.1); });
    addMenuAction(menu, "volumeDown", tr("Volume Down"), QKeySequence(Qt::CTRL | Qt::Key_Down),
                  [nudge] { nudge(-0.1); });
    if (!AudioController::available()) {
        QAction* note = menu->addAction(tr("(this build has no sound)"));
        note->setEnabled(false);
    }
}

// The bar under the viewer: the slider across the whole width, then the buttons, the frame and time
// readouts, and the loop and speed settings.
void MainWindow::buildTransport() {
    PlaybackController& playback = session_.playback();

    auto* panel = new QWidget(this);
    panel->setObjectName(QStringLiteral("transport"));
    panel->setAttribute(Qt::WA_StyledBackground, true);
    panel->setStyleSheet(QStringLiteral("#transport { background: #2a2a2d; border-top: 1px solid #1b1b1d; }"));
    auto* column = new QVBoxLayout(panel);
    column->setContentsMargins(8, 4, 8, 6);
    column->setSpacing(2);

    // The global timeline covers every frame, the range timeline only the playback range, so a
    // short range can be scrubbed finely.
    const auto timelineRow = [this, panel, column, &playback](const QString& caption, BookmarkSlider*& slider,
                                                             QWidget*& row) {
        row = new QWidget(panel);
        auto* line = new QHBoxLayout(row);
        line->setContentsMargins(0, 0, 0, 0);
        auto* label = new QLabel(caption, row);
        QFont small = label->font();
        small.setPointSizeF(small.pointSizeF() * 0.78);
        small.setBold(true);
        label->setFont(small);
        label->setStyleSheet(QStringLiteral("color: #8c8c90;"));
        label->setFixedWidth(QFontMetrics(small).horizontalAdvance(QStringLiteral("GLOBAL")) + 8);
        line->addWidget(label);
        slider = new BookmarkSlider(row);
        slider->setEnabled(false);
        line->addWidget(slider, 1);
        column->addWidget(row);
        connect(slider, &QSlider::valueChanged, this, [&playback](int value) {
            playback.pause();
            playback.seek(value);
        });
    };
    timelineRow(tr("GLOBAL"), slider_, globalRow_);
    slider_->setToolTip(tr("Global timeline: every frame. Yellow: frame bookmark, blue: range bookmark, "
                           "green: frame with drawings or a note."));
    timelineRow(tr("RANGE"), rangeSlider_, rangeRow_);
    rangeSlider_->setObjectName(QStringLiteral("timeline.range"));
    rangeSlider_->setToolTip(tr("Range timeline: only the playback range (I / O), for fine scrubbing."));

    auto* row = new QHBoxLayout;
    row->setSpacing(2);
    const auto button = [this, panel, row](const char* id) {
        auto* tool = new QToolButton(panel);
        tool->setDefaultAction(findChild<QAction*>(QStringLiteral("action.") + QLatin1String(id)));
        tool->setAutoRaise(true);
        tool->setIconSize(QSize(18, 18));
        row->addWidget(tool);
        return tool;
    };
    button("goToStart");
    button("previousFrame");
    button("play")->setIconSize(QSize(24, 24));
    button("nextFrame");
    button("goToEnd");
    row->addSpacing(8);
    row->addWidget(separatorLine());

    const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    QFont big = fixed;
    big.setPointSizeF(std::max(10.5, QFontInfo(fixed).pointSizeF() * 1.25));
    big.setBold(true);

    frameLabel_ = new QLabel(panel);
    frameLabel_->setObjectName(QStringLiteral("readout.frame"));
    frameLabel_->setFont(big);
    frameLabel_->setMinimumWidth(QFontMetrics(big).horizontalAdvance(QStringLiteral("0000 / 0000")));
    frameLabel_->setToolTip(tr("Current frame / total frames. Change how it is counted in View \u2192 Frame Numbering."));
    row->addWidget(readout(tr("FRAME"), frameLabel_, &frameCaption_));
    row->addWidget(separatorLine());

    timeLabel_ = new QLabel(panel);
    timeLabel_->setObjectName(QStringLiteral("readout.time"));
    timeLabel_->setFont(big);
    timeLabel_->setToolTip(tr("Time of the current frame / length of the whole clip (minutes:seconds.milliseconds)"));
    row->addWidget(readout(tr("TIME / DURATION"), timeLabel_));
    row->addWidget(separatorLine());

    rangeLabel_ = new QLabel(panel);
    rangeLabel_->setObjectName(QStringLiteral("readout.range"));
    QFont medium = fixed;
    medium.setPointSizeF(std::max(9.0, QFontInfo(fixed).pointSizeF() * 1.05));
    rangeLabel_->setFont(medium);
    rangeLabel_->setToolTip(tr("The part that plays. Set it with I and O, reset it in Bookmarks \u2192 Reset Playback Range."));
    row->addWidget(readout(tr("RANGE (I / O)"), rangeLabel_));
    row->addStretch(1);

    loopCombo_ = new QComboBox(panel);
    loopCombo_->setObjectName(QStringLiteral("transport.loop"));
    loopCombo_->addItems({tr("Play once"), tr("Loop"), tr("Ping-pong")});
    loopCombo_->setCurrentIndex(1);  // matches PlaybackController's default
    loopCombo_->setToolTip(tr("What happens at the end of the range"));
    loopCombo_->setFocusPolicy(Qt::NoFocus);
    connect(loopCombo_, &QComboBox::currentIndexChanged, this,
            [&playback](int index) { playback.setLoopMode(static_cast<LoopMode>(index)); });
    // Loop mode, stepped playback (bookmarks only) and speed share one column to keep the bar narrow.
    auto* playMode = new QWidget(panel);
    auto* playModeRow = new QHBoxLayout(playMode);
    playModeRow->setContentsMargins(0, 0, 0, 0);
    playModeRow->setSpacing(4);
    playModeRow->addWidget(loopCombo_);
    auto* stepped = new QToolButton(playMode);
    steppedAction_->setIconText(tr("Stepped"));
    stepped->setDefaultAction(steppedAction_);
    stepped->setToolButtonStyle(Qt::ToolButtonTextOnly);
    playModeRow->addWidget(stepped);

    speedCombo_ = new QComboBox(panel);
    speedCombo_->setObjectName(QStringLiteral("transport.speed"));
    for (const double value : {0.25, 0.5, 1.0, 2.0, 4.0}) {
        speedCombo_->addItem(QStringLiteral("%1×").arg(value), value);
    }
    speedCombo_->setCurrentIndex(2);
    speedCombo_->setFocusPolicy(Qt::NoFocus);
    speedCombo_->setToolTip(tr("Playback speed (Ctrl+1 normal, Ctrl+2 half, Ctrl+[ / Ctrl+] slower / faster)"));
    connect(speedCombo_, &QComboBox::currentIndexChanged, this, [this, &playback](int index) {
        if (index >= 0) playback.setSpeed(speedCombo_->itemData(index).toDouble());
    });
    playModeRow->addWidget(speedCombo_);
    row->addWidget(readout(tr("LOOP · STEPPED · SPEED"), playMode));

    if (QAction* mute = findChild<QAction*>(QStringLiteral("action.mute"))) {
        const auto showMute = [this, mute](bool muted) {
            mute->setIcon(style()->standardIcon(muted ? QStyle::SP_MediaVolumeMuted : QStyle::SP_MediaVolume));
        };
        showMute(mute->isChecked());
        connect(mute, &QAction::toggled, this, showMute);
        auto* tool = new QToolButton(panel);
        tool->setDefaultAction(mute);
        tool->setAutoRaise(true);
        tool->setIconSize(QSize(20, 20));

        volumeSlider_ = new QSlider(Qt::Horizontal, panel);
        volumeSlider_->setObjectName(QStringLiteral("transport.volume"));
        volumeSlider_->setRange(0, 100);
        volumeSlider_->setValue(qRound(audio_->volume() * 100.0));
        volumeSlider_->setFixedWidth(72);
        volumeSlider_->setFocusPolicy(Qt::NoFocus);
        volumeSlider_->setToolTip(tr("Volume (Ctrl+Up / Ctrl+Down)"));
        volumeLabel_ = new QLabel(panel);
        volumeLabel_->setStyleSheet(QStringLiteral("color: #8c8c90;"));
        volumeLabel_->setMinimumWidth(QFontMetrics(volumeLabel_->font()).horizontalAdvance(QStringLiteral("100")));
        volumeLabel_->setText(QString::number(volumeSlider_->value()));
        connect(volumeSlider_, &QSlider::valueChanged, this, [this](int value) { setVolumePercent(value); });
        auto* volume = new QWidget(panel);
        auto* volumeRow = new QHBoxLayout(volume);
        volumeRow->setContentsMargins(0, 0, 0, 0);
        volumeRow->setSpacing(2);
        volumeRow->addWidget(tool);
        volumeRow->addWidget(volumeSlider_);
        volumeRow->addWidget(volumeLabel_);
        row->addWidget(readout(tr("VOLUME %"), volume));
    }
    column->addLayout(row);

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(area_, 1);
    layout->addWidget(panel);
    setCentralWidget(central);
}

void MainWindow::buildTools() {
    QToolBar* bar = addToolBar(tr("Drawing"));
    bar->setObjectName(QStringLiteral("toolbar.drawing"));
    bar->setMovable(false);
    bar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    const auto caption = [this, bar](const QString& text) {
        auto* label = new QLabel(text, this);
        label->setStyleSheet(QStringLiteral("color: #8c8c90; padding: 0 4px 0 8px;"));
        bar->addWidget(label);
    };
    caption(tr("Tool"));

    auto* group = new QActionGroup(this);
    const struct {
        const char* id;
        QString label;
        ViewerWidget::Tool tool;
        Qt::Key key;
    } tools[] = {
        {"navigate", tr("Move"), ViewerWidget::Tool::Navigate, Qt::Key_V},
        {"pencil", tr("Pencil"), ViewerWidget::Tool::Pencil, Qt::Key_P},
        {"arrow", tr("Arrow"), ViewerWidget::Tool::Arrow, Qt::Key_A},
        {"ellipse", tr("Ellipse"), ViewerWidget::Tool::Ellipse, Qt::Key_E},
        {"rectangle", tr("Rectangle"), ViewerWidget::Tool::Rectangle, Qt::Key_R},
        {"text", tr("Text"), ViewerWidget::Tool::Text, Qt::Key_T},
        {"eraser", tr("Eraser"), ViewerWidget::Tool::Eraser, Qt::Key_X},
        {"select", tr("Select"), ViewerWidget::Tool::Select, Qt::Key_S},
        {"eyedropper", tr("Eyedropper"), ViewerWidget::Tool::Eyedropper, Qt::Key_D},
        {"laser", tr("Laser"), ViewerWidget::Tool::Laser, Qt::Key_L},
    };
    for (const auto& entry : tools) {
        QAction* action = bar->addAction(entry.label);
        action->setObjectName(QStringLiteral("tool.") + QLatin1String(entry.id));
        action->setCheckable(true);
        action->setActionGroup(group);
        action->setShortcut(QKeySequence(entry.key));
        action->setChecked(entry.tool == ViewerWidget::Tool::Navigate);
        addAction(action);  // the key works even when the toolbar is hidden
        connect(action, &QAction::triggered, this, [this, tool = entry.tool] { setTool(static_cast<int>(tool)); });
    }
    bar->addSeparator();

    caption(tr("Colour"));
    colorCombo_ = new QComboBox(this);
    const struct {
        const char* label;
        QColor color;
    } colors[] = {
        {"Red", QColor(255, 48, 48)},
        {"Yellow", QColor(255, 220, 0)},
        {"Cyan", QColor(48, 224, 255)},
        {"White", QColor(255, 255, 255)},
        {"Black", QColor(0, 0, 0)},
    };
    for (const auto& entry : colors) {
        QPixmap swatch(16, 16);
        swatch.fill(entry.color);
        colorCombo_->addItem(QIcon(swatch), tr(entry.label), QVariant::fromValue(entry.color));
    }
    colorCombo_->setToolTip(tr("Colour of new drawings (the Eyedropper adds a picked colour)"));
    bar->addWidget(colorCombo_);

    caption(tr("Line"));
    widthCombo_ = new QComboBox(this);
    widthCombo_->addItem(tr("Thin"), 0.5);
    widthCombo_->addItem(tr("Medium"), 1.0);
    widthCombo_->addItem(tr("Thick"), 2.0);
    widthCombo_->setCurrentIndex(1);
    widthCombo_->setToolTip(tr("Line width of new drawings"));
    bar->addWidget(widthCombo_);

    opacityCombo_ = new QComboBox(this);
    for (const int percent : {100, 75, 50, 25}) opacityCombo_->addItem(tr("%1% opaque").arg(percent), percent / 100.0);
    opacityCombo_->setToolTip(tr("Opacity of new drawings"));
    bar->addWidget(opacityCombo_);

    fillCombo_ = new QComboBox(this);
    fillCombo_->addItem(tr("No fill"), 0.0);
    for (const int percent : {25, 50, 75, 100}) fillCombo_->addItem(tr("Fill %1%").arg(percent), percent / 100.0);
    fillCombo_->setToolTip(tr("Fill of rectangles, ellipses and text boxes"));
    bar->addWidget(fillCombo_);

    smoothCombo_ = new QComboBox(this);
    smoothCombo_->addItem(tr("No smoothing"), 0);
    smoothCombo_->addItem(tr("Smooth low"), 1);
    smoothCombo_->addItem(tr("Smooth high"), 2);
    smoothCombo_->setToolTip(tr("Smoothing of pencil lines"));
    bar->addWidget(smoothCombo_);

    for (QComboBox* combo : {colorCombo_, widthCombo_, opacityCombo_, fillCombo_, smoothCombo_}) {
        connect(combo, &QComboBox::currentIndexChanged, this, [this] { updateStrokeStyle(); });
        combo->setFocusPolicy(Qt::NoFocus);  // keys stay with playback after a pick
    }

    updateStrokeStyle();
}

void MainWindow::buildBookmarks() {
    bookmarkList_ = new QListWidget(this);
    bookmarkList_->setObjectName(QStringLiteral("bookmarks.list"));
    connect(bookmarkList_, &QListWidget::itemDoubleClicked, this, &MainWindow::onBookmarkActivated);
    auto* dock = new QDockWidget(tr("Bookmarks"), this);
    dock->setObjectName(QStringLiteral("dock.bookmarks"));
    dock->setWidget(bookmarkList_);
    addDockWidget(Qt::RightDockWidgetArea, dock);

    QMenu* menu = menuBar()->addMenu(tr("&Bookmarks"));
    addMenuAction(menu, "toggleFrameBookmark", tr("Toggle Frame Bookmark"), QKeySequence(Qt::Key_B),
                  [this] { session_.toggleFrameBookmark(); });
    addMenuAction(menu, "toggleRangeBookmark", tr("Toggle Range Bookmark"), QKeySequence(Qt::SHIFT | Qt::Key_B),
                  [this] { session_.toggleRangeBookmark(); });
    menu->addSeparator();
    addMenuAction(menu, "previousBookmark", tr("Previous Bookmark"), QKeySequence(Qt::Key_BracketLeft),
                  [this] { session_.goToAdjacentBookmark(-1); });
    addMenuAction(menu, "nextBookmark", tr("Next Bookmark"), QKeySequence(Qt::Key_BracketRight),
                  [this] { session_.goToAdjacentBookmark(1); });
    menu->addSeparator();
    addMenuAction(menu, "setRangeStart", tr("Set Range Start Here"), QKeySequence(Qt::Key_I),
                  [this] { session_.setRangeStartHere(); });
    addMenuAction(menu, "setRangeEnd", tr("Set Range End Here"), QKeySequence(Qt::Key_O),
                  [this] { session_.setRangeEndHere(); });
    addMenuAction(menu, "resetRange", tr("Reset Playback Range"), QKeySequence(), [this] { session_.resetRange(); });
    menu->addSeparator();

    // Delete removes the selected bookmark only while the list has the focus.
    QAction* remove = addMenuAction(menu, "removeBookmark", tr("Remove Selected Bookmark"),
                                    QKeySequence(Qt::Key_Delete), [this] { removeSelectedBookmark(); });
    remove->setShortcutContext(Qt::WidgetShortcut);
    bookmarkList_->addAction(remove);
    addMenuAction(menu, "clearBookmarks", tr("Clear All Bookmarks"), QKeySequence(),
                  [this] { session_.clearBookmarks(); });

    menu->addSeparator();
    QMenu* filter = menu->addMenu(tr("Show in the List"));
    auto* filterGroup = new QActionGroup(this);
    const struct {
        const char* id;
        QString label;
        BookmarkFilter value;
    } filters[] = {
        {"filterAll", tr("All bookmarks"), BookmarkFilter::All},
        {"filterDrawings", tr("Only frames with drawings"), BookmarkFilter::WithDrawings},
        {"filterNotes", tr("Only frames with notes"), BookmarkFilter::WithNotes},
    };
    for (const auto& entry : filters) {
        QAction* action = filter->addAction(entry.label);
        action->setObjectName(QStringLiteral("action.") + QLatin1String(entry.id));
        action->setCheckable(true);
        action->setActionGroup(filterGroup);
        action->setChecked(entry.value == session_.bookmarkFilter());
        connect(action, &QAction::triggered, this, [this, value = entry.value] { session_.setBookmarkFilter(value); });
    }
}

// Facts about what the active viewer shows: size, rate, length, and how much has been reviewed.
void MainWindow::buildInfo() {
    auto* panel = new QWidget(this);
    auto* form = new QFormLayout(panel);
    form->setLabelAlignment(Qt::AlignRight);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(4);
    const auto row = [this, form](const QString& label, const char* id) {
        auto* value = new QLabel(this);
        value->setObjectName(QStringLiteral("info.") + QLatin1String(id));
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setWordWrap(true);
        auto* name = new QLabel(label, this);
        name->setStyleSheet(QStringLiteral("color: #8c8c90;"));
        form->addRow(name, value);
        return value;
    };
    info_.name = row(tr("Name"), "name");
    info_.file = row(tr("File"), "file");
    info_.resolution = row(tr("Resolution"), "resolution");
    info_.fps = row(tr("Frame rate"), "fps");
    info_.frames = row(tr("Total frames"), "frames");
    info_.duration = row(tr("Duration"), "duration");
    info_.range = row(tr("Playback range"), "range");
    info_.annotated = row(tr("Reviewed"), "annotated");
    info_.bookmarks = row(tr("Bookmarks"), "bookmarks");
    info_.annotated->setToolTip(tr("Frames with drawings or a note. Alt+Left / Alt+Right jumps between them."));

    // Scrolls rather than cutting text off when the panel is small (a laptop screen).
    auto* scroll = new QScrollArea(this);
    scroll->setWidget(panel);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setMinimumWidth(215);
    auto* dock = new QDockWidget(tr("Info"), this);
    dock->setObjectName(QStringLiteral("dock.info"));
    dock->setWidget(scroll);
    addDockWidget(Qt::RightDockWidgetArea, dock);
}

void MainWindow::buildHelpMenu() {
    QMenu* menu = menuBar()->addMenu(tr("&Help"));
    addMenuAction(menu, "shortcuts", tr("Keyboard Shortcuts (View and Change)…"), QKeySequence(Qt::Key_F1),
                  [this] { showShortcuts(); });
    addMenuAction(menu, "about", tr("About KenaePRO"), QKeySequence(), [this] {
        QMessageBox::about(this, tr("About KenaePRO"),
                           tr("<b>KenaePRO %1</b><p>Frame-by-frame video review and annotation.</p>")
                               .arg(QCoreApplication::applicationVersion()));
    });
}

// Project and Timeline share the left side as tabs; Info, Notes and Bookmarks are stacked on the right.
void MainWindow::arrangeDocks() {
    auto* project = findChild<QDockWidget*>(QStringLiteral("dock.project"));
    auto* timeline = findChild<QDockWidget*>(QStringLiteral("dock.timeline"));
    auto* info = findChild<QDockWidget*>(QStringLiteral("dock.info"));
    auto* notes = findChild<QDockWidget*>(QStringLiteral("dock.notes"));
    auto* bookmarks = findChild<QDockWidget*>(QStringLiteral("dock.bookmarks"));
    if (!project || !timeline || !info || !notes || !bookmarks) return;
    tabifyDockWidget(project, timeline);
    project->raise();
    setTabPosition(Qt::LeftDockWidgetArea, QTabWidget::North);
    // The sizes only stick once the window is laid out at its real size.
    QTimer::singleShot(0, this, [this, project, info, notes, bookmarks] {
        if (layoutRestored_) return;  // the user's own arrangement wins
        resizeDocks({project, info}, {220, 280}, Qt::Horizontal);
        resizeDocks({info, notes, bookmarks}, {270, 200, 300}, Qt::Vertical);
    });
}

// Every action with a key shows it in its tooltip, so the buttons teach their shortcuts.
void MainWindow::addShortcutTooltips() {
    static const char* const kBaseTip = "kenaeproBaseToolTip";
    for (QAction* action : findChildren<QAction*>()) {
        if (action->text().isEmpty()) continue;
        if (!action->property(kBaseTip).isValid()) {  // the tooltip before any key was added to it
            QString text = action->toolTip();
            text.remove(QLatin1Char('&'));
            action->setProperty(kBaseTip, text);
        }
        const QString base = action->property(kBaseTip).toString();
        action->setToolTip(action->shortcut().isEmpty()
                               ? base
                               : tr("%1  (%2)").arg(base, action->shortcut().toString(QKeySequence::NativeText)));
    }
}

QVector<MainWindow::Command> MainWindow::commands() const {
    QVector<Command> result;
    QSet<QString> seen;
    const auto add = [&](const QString& menu, QAction* action) {
        if (action->isSeparator() || action->text().isEmpty()) return;
        QString text = action->text();
        text.remove(QLatin1Char('&'));
        const QString id = action->objectName().isEmpty() ? QStringLiteral("menu.%1.%2").arg(menu, text)
                                                          : action->objectName();
        if (seen.contains(id)) return;
        seen.insert(id);
        result.push_back(Command{id, menu, action});
    };
    std::function<void(const QString&, QMenu*)> walk = [&](const QString& name, QMenu* menu) {
        if (menu == recentMenu_) return;  // file names, rebuilt all the time
        for (QAction* action : menu->actions()) {
            if (action->menu()) walk(name, action->menu()); else add(name, action);
        }
    };
    for (QAction* top : menuBar()->actions()) {
        if (top->menu()) walk(top->text().remove(QLatin1Char('&')), top->menu());
    }
    if (auto* tools = findChild<QToolBar*>(QStringLiteral("toolbar.drawing"))) {
        for (QAction* action : tools->actions()) {
            if (action->objectName().startsWith(QLatin1String("tool."))) add(tr("Drawing tools"), action);
        }
    }
    return result;
}

void MainWindow::applyShortcutOverrides() {
    const QHash<QString, QString> changed = prefs_.shortcuts();
    for (const Command& command : commands()) {
        const auto it = changed.constFind(command.id);
        command.action->setShortcut(it != changed.cend() ? QKeySequence(it.value(), QKeySequence::PortableText)
                                                         : defaultShortcuts_.value(command.id));
    }
    addShortcutTooltips();
}

void MainWindow::restoreLayout() {
    const QByteArray state = prefs_.windowState();
    if (state.isEmpty()) return;
    restoreGeometry(prefs_.windowGeometry());
    layoutRestored_ = restoreState(state);
}

// Every panel back where it starts, docked and visible.
void MainWindow::resetLayout() {
    prefs_.clearWindowLayout();
    layoutRestored_ = false;
    const struct {
        const char* id;
        Qt::DockWidgetArea area;
    } places[] = {
        {"dock.project", Qt::LeftDockWidgetArea},  {"dock.timeline", Qt::LeftDockWidgetArea},
        {"dock.info", Qt::RightDockWidgetArea},    {"dock.notes", Qt::RightDockWidgetArea},
        {"dock.bookmarks", Qt::RightDockWidgetArea},
    };
    if (auto* tools = findChild<QToolBar*>(QStringLiteral("toolbar.drawing"))) tools->show();
    for (const auto& place : places) {
        QDockWidget* dock = findChild<QDockWidget*>(QLatin1String(place.id));
        if (!dock) continue;
        removeDockWidget(dock);
        dock->setFloating(false);
        addDockWidget(place.area, dock);
        dock->show();
    }
    arrangeDocks();
}

void MainWindow::setAlwaysOnTop(bool onTop) {
    prefs_.setAlwaysOnTop(onTop);
    const bool visible = isVisible();
    setWindowFlag(Qt::WindowStaysOnTopHint, onTop);
    if (visible) show();  // changing a window flag hides the window
}

void MainWindow::rotateViewers(int quarterTurns) {
    for (int i = 0; i < 2; ++i) area_->viewer(i)->setRotation(area_->viewer(i)->rotation() + quarterTurns);
    statusBar()->showMessage(tr("Rotation %1°").arg(area_->viewer(0)->rotation() * 90), 3000);
}

// --- refreshing ------------------------------------------------------------------------------

void MainWindow::refreshTree() {
    tree_->clear();
    const Project& project = session_.project();
    const QString active = session_.activeSourceId();

    // Folders can come in any order, so create every item first and link them afterwards.
    QHash<QString, QTreeWidgetItem*> folderItems;
    for (const Folder& folder : project.folders) {
        auto* item = new QTreeWidgetItem;
        item->setText(0, folder.name);
        item->setData(0, kIdRole, folder.id);
        item->setData(0, kKindRole, KindFolder);
        folderItems.insert(folder.id, item);
    }
    for (const Folder& folder : project.folders) {
        QTreeWidgetItem* item = folderItems.value(folder.id);
        if (QTreeWidgetItem* parent = folderItems.value(folder.parentId, nullptr)) {
            parent->addChild(item);
        } else {
            tree_->addTopLevelItem(item);
        }
    }

    const auto addLeaf = [&](const QString& id, const QString& name, const QString& parentId, ItemKind kind) {
        auto* item = new QTreeWidgetItem;
        if (kind == KindSource) item->setToolTip(0, session_.sourcePath(id));
        QStringList letters;
        if (session_.viewerTargetId(0) == id) letters << QStringLiteral("A");
        if (session_.viewerCount() > 1 && session_.viewerTargetId(1) == id) letters << QStringLiteral("B");
        if (!letters.isEmpty()) {
            item->setText(1, letters.join(QLatin1Char(' ')));
            item->setForeground(1, letters.first() == QLatin1String("A") ? QColor(74, 163, 255) : QColor(255, 166, 74));
            QFont bold = item->font(1);
            bold.setBold(true);
            item->setFont(1, bold);
            item->setToolTip(1, tr("Shown in viewer %1").arg(letters.join(tr(" and "))));
        }
        item->setText(0, name);
        item->setData(0, kIdRole, id);
        item->setData(0, kKindRole, kind);
        const bool shown = id == active || (session_.viewerTargetId(0) == id) ||
                           (session_.viewerCount() > 1 && session_.viewerTargetId(1) == id);
        if (shown) {
            QFont font = item->font(0);
            font.setBold(true);
            item->setFont(0, font);
        }
        if (QTreeWidgetItem* parent = folderItems.value(parentId, nullptr)) {
            parent->addChild(item);
        } else {
            tree_->addTopLevelItem(item);
        }
    };
    for (const Source& source : project.sources) addLeaf(source.id, source.name, source.parentId, KindSource);
    for (const Timeline& timeline : project.timelines) {
        addLeaf(timeline.id, timeline.isDefault ? tr("%1 (default)").arg(timeline.name) : timeline.name,
                timeline.parentId, KindTimeline);
    }
    tree_->expandAll();
    refreshBadges();
}

void MainWindow::refreshViewer(int viewer) {
    if (viewer == 1 && splitLayout()) {  // B is drawn past the divider of the first viewer
        ViewerWidget* widget = area_->viewer(0);
        const QImage image = session_.currentImage(1);
        widget->setSplitImage(image);
        imageHeight_[1] = image.height();
        widget->setSplitStrokes(session_.strokesToShow(1), session_.strokesAreEditable(1));
        return;
    }
    ViewerWidget* widget = area_->viewer(viewer);
    const QImage image = session_.currentImage(viewer);
    widget->setImage(image);
    imageHeight_[viewer] = image.height();
    widget->setStrokes(session_.strokesToShow(viewer), session_.strokesAreEditable(viewer));
    widget->setGhostStrokes(ghostDrawings_ ? session_.ghostStrokes(viewer) : QVector<Stroke>());

    QVector<ViewerWidget::Ghost> ghosts;
    for (int distance = 1; distance <= ghostFrames_; ++distance) {
        for (const int delta : {-distance, distance}) {
            const QImage ghost = session_.ghostImage(viewer, delta);
            if (!ghost.isNull()) ghosts.push_back(ViewerWidget::Ghost{ghost, ghostOpacity_ / distance});
        }
    }
    widget->setGhosts(ghosts);
    updateStrokeStyle();
}

void MainWindow::refreshViews() {
    for (int i = 0; i < session_.viewerCount(); ++i) refreshViewer(i);
}

void MainWindow::refreshNote() {
    const bool hasSource = session_.locationSource() != nullptr;
    const QString text = session_.currentNote();
    if (text == noteEdit_->toPlainText() && hasSource == noteEdit_->isEnabled()) return;
    updatingNote_ = true;
    noteEdit_->setPlainText(text);
    noteEdit_->setEnabled(hasSource);
    updatingNote_ = false;
}

void MainWindow::refreshBookmarks() {
    // List items carry the index of their entry in bookmarkEntries_.
    bookmarkList_->clear();
    bookmarkEntries_ = session_.bookmarkEntries();
    const double fps = session_.playback().fps();
    const int offset = session_.viewerOffset(session_.activeViewer());
    for (int i = 0; i < bookmarkEntries_.size(); ++i) {
        const BookmarkEntry& entry = bookmarkEntries_[i];
        const int length = entry.end - entry.start + 1;
        const QString text =
            entry.isRange
                ? tr("Range %1–%2   %3   (%4 frames, %5)")
                      .arg(entry.start - offset + 1)
                      .arg(entry.end - offset + 1)
                      .arg(formatDuration(entry.start - offset, fps))
                      .arg(length)
                      .arg(spokenDuration(length, fps))
                : tr("Frame %1   %2").arg(entry.start - offset + 1).arg(formatDuration(entry.start - offset, fps));
        auto* item = new QListWidgetItem(text, bookmarkList_);
        item->setData(Qt::UserRole, i);
    }
    QVector<int> frames;
    QVector<RangeBookmark> ranges;
    session_.bookmarkMarks(session_.activeViewer(), frames, ranges);
    slider_->setMarks(frames, ranges);
    rangeSlider_->setMarks(frames, ranges);
    // Stepped playback visits the frame bookmarks and the start of every range bookmark.
    QVector<int> keys = frames;
    for (const RangeBookmark& range : ranges) keys.push_back(range.start);
    session_.playback().setSteppedKeys(keys);
}

void MainWindow::refreshFrameLabel() {
    static const char* const captions[] = {QT_TR_NOOP("FRAME"), QT_TR_NOOP("IMAGE NUMBER"),
                                           QT_TR_NOOP("FRAME IN RANGE"), QT_TR_NOOP("TIMECODE")};
    frameCaption_->setText(tr(captions[static_cast<int>(numbering_)]));
    const int count = session_.viewerFrameCount();
    if (count <= 0) {
        const QString none = QStringLiteral("–");
        frameLabel_->setText(none);
        timeLabel_->setText(none);
        rangeLabel_->setText(none);
        return;
    }
    const PlaybackController& playback = session_.playback();
    const double fps = playback.fps();
    frameLabel_->setText(session_.frameLabel(numbering_));
    timeLabel_->setText(tr("%1 / %2").arg(formatDuration(session_.viewerFrame(), fps), formatDuration(count, fps)));
    const int offset = session_.viewerOffset(session_.activeViewer());
    const int first = std::clamp(playback.rangeStart() - offset, 0, count - 1);
    const int last = std::clamp(playback.rangeEnd() - offset, first, count - 1);
    rangeLabel_->setText(tr("%1 – %2  (%3 f)").arg(first + 1).arg(last + 1).arg(last - first + 1));
}

void MainWindow::refreshInfo() {
    const QString none = QStringLiteral("—");
    const QString kind = session_.viewerTargetKind();
    const QString id = session_.viewerTargetId();
    const int count = session_.viewerFrameCount();
    const PlaybackController& playback = session_.playback();
    const double fps = playback.fps();

    const QVector<int> annotated = session_.annotatedFrames();
    slider_->setAnnotationMarks(annotated);
    rangeSlider_->setAnnotationMarks(annotated);
    slider_->setPlaybackRange(playback.rangeStart(), playback.rangeEnd());

    if (kind.isEmpty() || count <= 0) {
        for (QLabel* label : {info_.name, info_.file, info_.resolution, info_.fps, info_.frames, info_.duration,
                              info_.range, info_.annotated, info_.bookmarks}) {
            label->setText(none);
            label->setToolTip(QString());
        }
        statusInfo_->setText(tr("No media open"));
        return;
    }

    if (kind == QLatin1String("timeline")) {
        const Timeline* timeline = session_.project().findTimeline(id);
        info_.name->setText(timeline ? tr("%1 (timeline, %2)")
                                           .arg(timeline->name,
                                                countOf(static_cast<int>(timeline->clips.size()), tr("clip"), tr("clips")))
                                     : none);
        info_.file->setText(none);
        info_.file->setToolTip(QString());
    } else {
        const Source* source = session_.project().findSource(id);
        info_.name->setText(source ? breakable(source->name) : none);
        const QString path = session_.sourcePath(id);
        info_.file->setText(path.isEmpty() ? none : breakable(QFileInfo(path).fileName()));
        info_.file->setToolTip(path);
    }

    const QSize size = session_.viewerFrameSize();
    const QString resolution = size.isEmpty() ? none : tr("%1 × %2").arg(size.width()).arg(size.height());
    info_.resolution->setText(resolution);
    info_.fps->setText(fpsText(fps));
    info_.frames->setText(QString::number(count));
    info_.duration->setText(tr("%1  (%2)").arg(formatDuration(count, fps), spokenDuration(count, fps)));

    const int offset = session_.viewerOffset(session_.activeViewer());
    const int first = std::clamp(playback.rangeStart() - offset, 0, count - 1);
    const int last = std::clamp(playback.rangeEnd() - offset, first, count - 1);
    const int length = last - first + 1;
    info_.range->setText(length == count ? tr("Whole clip")
                                         : tr("%1 – %2  (%3 frames, %4)")
                                               .arg(first + 1)
                                               .arg(last + 1)
                                               .arg(length)
                                               .arg(spokenDuration(length, fps)));
    info_.annotated->setText(annotated.isEmpty()
                                 ? tr("No drawings or notes yet")
                                 : tr("%1 with drawings or notes")
                                       .arg(countOf(static_cast<int>(annotated.size()), tr("frame"), tr("frames"))));

    QVector<int> frameMarks;
    QVector<RangeBookmark> rangeMarks;
    session_.bookmarkMarks(session_.activeViewer(), frameMarks, rangeMarks);
    info_.bookmarks->setText(countOf(static_cast<int>(frameMarks.size()), tr("frame"), tr("frames")) +
                             QStringLiteral(", ") +
                             countOf(static_cast<int>(rangeMarks.size()), tr("range"), tr("ranges")));

    statusInfo_->setText(QStringLiteral("%1  ·  %2  ·  %3  ·  %4")
                             .arg(resolution, fpsText(fps), countOf(count, tr("frame"), tr("frames")),
                                  formatDuration(count, fps)));
}

void MainWindow::updateTitle() {
    QString name = tr("Untitled");
    if (!session_.projectPath().isEmpty()) name = QFileInfo(session_.projectPath()).completeBaseName();
    setWindowTitle(tr("%1[*] — KenaePRO").arg(name));
    setWindowModified(dirty_);
}

void MainWindow::updateStrokeStyle() {
    if (!colorCombo_ || !widthCombo_ || !opacityCombo_ || !fillCombo_ || !smoothCombo_) return;
    const QColor color = colorCombo_->currentData().value<QColor>();
    for (int i = 0; i < 2; ++i) {
        ViewerWidget* viewer = area_->viewer(i);
        viewer->setStrokeColor(color);
        // "Medium" is about 0.6% of the picture height, so it reads the same on a 480p and a 4K clip.
        const double base = std::max(2.0, imageHeight_[i] * 0.006);
        viewer->setStrokeWidth(widthCombo_->currentData().toDouble() * base);
        viewer->setStrokeOpacity(opacityCombo_->currentData().toDouble());
        viewer->setFillOpacity(fillCombo_->currentData().toDouble());
        viewer->setSmoothing(smoothCombo_->currentData().toInt());
    }
}

void MainWindow::applyViewState(int viewer) {
    const Source* source = session_.locationSource(viewer);
    if (!source || !source->panZoomEnabled) return;
    area_->viewer(viewer)->setViewState(ViewerWidget::ViewState{source->zoom, source->panX, source->panY});
}

void MainWindow::storeView(int viewer) {
    const ViewerWidget::ViewState state = area_->viewer(viewer)->viewState();
    session_.setSourceView(viewer, state.zoom, state.panX, state.panY);
}

void MainWindow::syncSpeedCombo() {
    const double speed = session_.playback().speed();
    const QSignalBlocker blocker(speedCombo_);
    for (int i = 0; i < speedCombo_->count(); ++i) {
        if (std::fabs(speedCombo_->itemData(i).toDouble() - speed) < 1e-6) {
            speedCombo_->setCurrentIndex(i);
            return;
        }
    }
    speedCombo_->addItem(QStringLiteral("%1×").arg(speed, 0, 'g', 3), speed);
    speedCombo_->setCurrentIndex(speedCombo_->count() - 1);
}

void MainWindow::updateRecentMenu() {
    recentMenu_->clear();
    const QStringList files = prefs_.recentFiles();
    for (const QString& path : files) {
        QAction* action = recentMenu_->addAction(QFileInfo(path).fileName());
        action->setToolTip(path);
        connect(action, &QAction::triggered, this, [this, path] {
            if (maybeSave()) openProjectFile(path);
        });
    }
    if (!files.isEmpty()) {
        recentMenu_->addSeparator();
        recentMenu_->addAction(tr("Clear List"), this, [this] {
            prefs_.clearRecentFiles();
            updateRecentMenu();
        });
    }
    recentMenu_->setEnabled(!files.isEmpty());
}

// --- reacting --------------------------------------------------------------------------------

void MainWindow::onFrameChanged(int frame) {
    for (BookmarkSlider* slider : {slider_, rangeSlider_}) {
        const QSignalBlocker blocker(slider);
        slider->setValue(frame);
    }
    refreshFrameLabel();
    refreshViews();
    refreshNote();
    // Playing across the clips of a timeline changes the source under the playhead.
    const QString source = session_.activeSourceId();
    if (source != lastSourceId_) {
        lastSourceId_ = source;
        refreshTree();
        refreshBookmarks();
        refreshInfo();
        for (int i = 0; i < 2; ++i) applyViewState(i);
    }
}

void MainWindow::onConfigured() {
    const int count = session_.playback().frameCount();
    slider_->blockSignals(true);
    slider_->setRange(0, std::max(0, count - 1));
    slider_->setEnabled(count > 1);
    slider_->blockSignals(false);
    {
        const QSignalBlocker blocker(rangeSlider_);
        const PlaybackController& playback = session_.playback();
        rangeSlider_->setRange(playback.rangeStart(), std::max(playback.rangeStart(), playback.rangeEnd()));
        rangeSlider_->setEnabled(playback.rangeEnd() > playback.rangeStart());
    }
    refreshFrameLabel();
    refreshInfo();
}

void MainWindow::onNoteEdited() {
    if (updatingNote_) return;
    session_.setCurrentNote(noteEdit_->toPlainText());
}

void MainWindow::onTreeActivated(QTreeWidgetItem* item) {
    if (!item) return;
    const int kind = item->data(0, kKindRole).toInt();
    const QString id = item->data(0, kIdRole).toString();
    QString error;
    if (kind == KindSource) {
        if (!session_.activateSource(id, &error)) showError(tr("Cannot open source"), error);
    } else if (kind == KindTimeline) {
        if (!session_.activateTimeline(id, &error)) showError(tr("Cannot show the timeline"), error);
    }
}

void MainWindow::showSourceInViewer(const QString& id, int viewer) {
    if (viewer > 0 && session_.viewerCount() < 2) session_.setViewerLayout(QStringLiteral("horizontal"));
    session_.setActiveViewer(viewer);
    QString error;
    if (!session_.activateSource(id, &error)) showError(tr("Cannot open source"), error);
}

void MainWindow::onTreeContextMenu(const QPoint& position) {
    QTreeWidgetItem* item = tree_->itemAt(position);
    if (!item) return;
    const int kind = item->data(0, kKindRole).toInt();
    const QString id = item->data(0, kIdRole).toString();
    const bool base = Project::isBaseFolder(id);

    QMenu menu(this);
    if (kind == KindSource) {
        menu.addAction(tr("Show as A (first viewer)"), this, [this, id] { showSourceInViewer(id, 0); });
        menu.addAction(tr("Show as B (second viewer, to compare)"), this, [this, id] { showSourceInViewer(id, 1); });
        menu.addAction(tr("Add to the Default Timeline"), this, [this, id] {
            const Timeline* timeline = session_.project().defaultTimeline();
            const QString timelineId = timeline ? timeline->id : session_.addTimeline(tr("Timeline 1"));
            session_.addClipToTimeline(timelineId, id);
        });
        menu.addAction(tr("Duplicate"), this, [this, id] { session_.duplicateSource(id); });
        menu.addSeparator();
    } else if (kind == KindTimeline) {
        menu.addAction(tr("Show in the Active Viewer"), this, [this, id] {
            QString error;
            if (!session_.activateTimeline(id, &error)) showError(tr("Cannot show the timeline"), error);
        });
        menu.addAction(tr("Set as Default"), this, [this, id] { session_.setDefaultTimeline(id); });
        menu.addSeparator();
    } else {
        menu.addAction(tr("New Folder"), this, [this, id] {
            bool ok = false;
            const QString name = QInputDialog::getText(this, tr("New Folder"), tr("Name:"), QLineEdit::Normal,
                                                       tr("New Folder"), &ok);
            if (!ok || name.trimmed().isEmpty()) return;
            if (!session_.project().addFolder(name.trimmed(), id).isEmpty()) session_.notifyProjectChanged();
        });
    }

    if (!base) {
        // The tree is rebuilt while a timeline plays, so keep the name, not the item.
        menu.addAction(tr("Rename…"), this, [this, id, current = item->text(0)] {
            bool ok = false;
            const QString name = QInputDialog::getText(this, tr("Rename"), tr("Name:"), QLineEdit::Normal,
                                                       current, &ok);
            if (ok) session_.renameItem(id, name);
        });
        QMenu* move = menu.addMenu(tr("Move to"));
        for (const Folder& folder : session_.project().folders) {
            if (folder.id == id) continue;
            move->addAction(folder.name, this, [this, id, target = folder.id] { session_.moveItem(id, target); });
        }
        menu.addAction(tr("Remove"), this, [this, id] {
            RemoveStatus status = session_.removeItem(id, false);
            if (status == RemoveStatus::SourceInTimelines) {
                const auto answer = QMessageBox::question(
                    this, tr("Remove source"),
                    tr("This source is used by a timeline. Remove it and its clips anyway?"));
                if (answer == QMessageBox::Yes) status = session_.removeItem(id, true);
            }
            if (status != RemoveStatus::Ok && status != RemoveStatus::SourceInTimelines) {
                showError(tr("Cannot remove"), removeStatusText(status));
            }
        });
    }
    menu.exec(tree_->viewport()->mapToGlobal(position));
}

void MainWindow::onStrokeFinished(int viewer, const Stroke& stroke) {
    session_.setActiveViewer(viewer);
    if (!session_.location()) return;
    session_.playback().pause();  // drawings belong to one paused frame
    session_.addStroke(stroke);
}

void MainWindow::onTextRequested(int viewer, const QPointF& normalised) {
    session_.setActiveViewer(viewer);
    if (!session_.location()) return;
    bool ok = false;
    const QString text = QInputDialog::getText(this, tr("Add Text"), tr("Text:"), QLineEdit::Normal, QString(), &ok);
    if (!ok || text.trimmed().isEmpty()) return;

    ViewerWidget* widget = widgetFor(viewer);
    Stroke stroke;
    stroke.type = Stroke::Type::Text;
    stroke.color = widget->strokeColor();
    stroke.color.setAlphaF(widget->strokeOpacity());
    stroke.width = widget->strokeWidth();
    stroke.fillOpacity = widget->fillOpacity();
    stroke.text = text;
    stroke.points = {normalised};
    onStrokeFinished(viewer, stroke);
}

void MainWindow::onColorPicked(const QColor& color) {
    // The picked colour gets its own entry at the end of the list, replaced by the next pick.
    const int last = colorCombo_->count() - 1;
    QPixmap swatch(16, 16);
    swatch.fill(color);
    if (colorCombo_->itemText(last) == tr("Picked")) {
        colorCombo_->setItemIcon(last, QIcon(swatch));
        colorCombo_->setItemData(last, QVariant::fromValue(color));
    } else {
        colorCombo_->addItem(QIcon(swatch), tr("Picked"), QVariant::fromValue(color));
    }
    colorCombo_->setCurrentIndex(colorCombo_->count() - 1);
    updateStrokeStyle();
}

void MainWindow::onBookmarkActivated(QListWidgetItem* item) {
    if (!item) return;
    const int index = item->data(Qt::UserRole).toInt();
    if (index < 0 || index >= bookmarkEntries_.size()) return;
    const BookmarkEntry& entry = bookmarkEntries_[index];
    PlaybackController& playback = session_.playback();
    playback.pause();
    if (entry.isRange) playback.setRange(entry.start, entry.end);  // a range bookmark also becomes the playback range
    playback.seek(entry.start);
}

void MainWindow::removeSelectedBookmark() {
    const QListWidgetItem* item = bookmarkList_->currentItem();
    if (!item) return;
    const int index = item->data(Qt::UserRole).toInt();
    if (index < 0 || index >= bookmarkEntries_.size()) return;
    session_.removeBookmarkEntry(bookmarkEntries_[index]);
}

void MainWindow::setTool(int tool) {
    for (int i = 0; i < 2; ++i) area_->viewer(i)->setTool(static_cast<ViewerWidget::Tool>(tool));
    // In the order of ViewerWidget::Tool.
    static const char* const hints[] = {
        QT_TR_NOOP("Move: drag to pan, scroll to zoom, 0 resets the view"),
        QT_TR_NOOP("Pencil: drag on the picture to draw"),
        QT_TR_NOOP("Arrow: drag from the tail to the head"),
        QT_TR_NOOP("Ellipse: drag a box around the ellipse"),
        QT_TR_NOOP("Rectangle: drag from one corner to the other"),
        QT_TR_NOOP("Text: click where the text goes"),
        QT_TR_NOOP("Eraser: drag across the drawings to remove"),
        QT_TR_NOOP("Select: click a drawing, then drag it or delete it from the Edit menu"),
        QT_TR_NOOP("Eyedropper: click the picture to take its colour"),
        QT_TR_NOOP("Laser: a pointer for presenting, it draws nothing"),
    };
    if (tool >= 0 && tool < static_cast<int>(std::size(hints))) statusBar()->showMessage(tr(hints[tool]), 6000);
}

// --- commands --------------------------------------------------------------------------------

bool MainWindow::maybeSave() {
    if (!dirty_ || !prefs_.confirmOnClose()) return true;
    const auto answer = QMessageBox::question(this, tr("Unsaved changes"),
                                              tr("Save the changes to this project?"),
                                              QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (answer == QMessageBox::Save) return saveProject();
    return answer == QMessageBox::Discard;
}

void MainWindow::openProject() {
    if (!maybeSave()) return;
    const QString path = QFileDialog::getOpenFileName(this, tr("Open Project"), QString(),
                                                      tr("Projects (*.kpro);;All files (*)"));
    if (!path.isEmpty()) openProjectFile(path);
}

void MainWindow::openProjectFile(const QString& path) {
    QString error;
    if (!session_.openProject(path, &error)) {
        showError(tr("Cannot open project"), error);
        return;
    }
    prefs_.addRecentFile(path);
    updateRecentMenu();
    activateFirstSource();
    if (!session_.loadWarnings().isEmpty()) {
        statusBar()->showMessage(tr("Opened with %1 warning(s): %2")
                                     .arg(session_.loadWarnings().size())
                                     .arg(session_.loadWarnings().first()),
                                 10000);
    }
}

void MainWindow::activateFirstSource() {
    if (session_.project().sources.isEmpty()) return;
    const QString id = session_.project().sources.first().id;
    QString error;
    if (!session_.activateSource(id, &error)) {
        statusBar()->showMessage(tr("Cannot open the first source: %1").arg(error), 10000);
    }
}

bool MainWindow::saveProject() {
    if (session_.projectPath().isEmpty()) return saveProjectAs();
    QString error;
    if (!session_.saveProject(QString(), &error)) {
        showError(tr("Cannot save project"), error);
        return false;
    }
    dirty_ = false;
    updateTitle();
    return true;
}

bool MainWindow::saveProjectAs() {
    QString path = QFileDialog::getSaveFileName(this, tr("Save Project"), QString(), tr("Projects (*.kpro)"));
    if (path.isEmpty()) return false;
    if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".kpro");
    QString error;
    if (!session_.saveProject(path, &error)) {
        showError(tr("Cannot save project"), error);
        return false;
    }
    prefs_.addRecentFile(path);
    updateRecentMenu();
    dirty_ = false;
    updateTitle();
    return true;
}

void MainWindow::importMedia() {
    importPaths(QFileDialog::getOpenFileNames(this, tr("Import Media"), QString(), mediaFilter()), true);
}

void MainWindow::importSequenceDialog() {
    const QString file = QFileDialog::getOpenFileName(this, tr("Import Image Sequence"), QString(),
                                                      tr("Images (*.png *.jpg *.jpeg *.bmp *.tif *.tiff *.webp);;All files (*)"));
    if (file.isEmpty()) return;
    QString error;
    const QString id = session_.importSequence(file, &error);
    if (id.isEmpty()) {
        showError(tr("Cannot import"), error);
        return;
    }
    if (!session_.activateSource(id, &error)) showError(tr("Cannot open source"), error);
}

void MainWindow::importFolderDialog() {
    const QString folder = QFileDialog::getExistingDirectory(this, tr("Import Folder"));
    if (!folder.isEmpty()) importPaths({folder}, false);
}

// Imports files, folders and image sequences, and shows the first thing that was added. A
// numbered image that has neighbours is a sequence; with `askAboutSequences` the user is asked.
void MainWindow::importPaths(const QStringList& paths, bool askAboutSequences) {
    QString firstId;
    QHash<QString, bool> sequenceAnswers;  // per sequence: import as one source or not
    const auto remember = [&firstId](const QString& id) {
        if (firstId.isEmpty() && !id.isEmpty()) firstId = id;
    };

    for (const QString& path : paths) {
        QString error;
        const QFileInfo info(path);
        if (info.isDir()) {
            const QStringList ids = session_.importFolder(path, &error);
            if (ids.isEmpty() && !error.isEmpty()) showError(tr("Cannot import"), error);
            if (!ids.isEmpty()) remember(ids.first());
            continue;
        }
        if (isImageFile(path)) {
            const std::optional<SequencePattern> pattern = sequenceFromFile(path);
            const int frames = pattern ? static_cast<int>(scanSequence(*pattern).size()) : 0;
            if (pattern && frames >= 2) {
                const QString key = pattern->pattern();
                const auto known = sequenceAnswers.constFind(key);
                if (known != sequenceAnswers.cend() && known.value()) continue;  // imported already
                bool asSequence = true;
                if (known != sequenceAnswers.cend()) {
                    asSequence = false;  // asked once for this sequence: the answer was no
                } else if (askAboutSequences) {
                    asSequence = QMessageBox::question(
                                     this, tr("Image sequence"),
                                     tr("%1 is one of %2 numbered images. Import them all as one image sequence?")
                                         .arg(info.fileName())
                                         .arg(frames)) == QMessageBox::Yes;
                }
                sequenceAnswers.insert(key, asSequence);
                if (asSequence) {
                    const QString id = session_.importSequence(path, &error);
                    if (id.isEmpty()) showError(tr("Cannot import"), error); else remember(id);
                    continue;
                }
            }
        }
        const QString id = session_.importFile(path, &error);
        if (id.isEmpty()) showError(tr("Cannot import"), error); else remember(id);
    }

    if (firstId.isEmpty()) return;
    QString error;
    if (!session_.activateSource(firstId, &error)) showError(tr("Cannot open source"), error);
}

void MainWindow::openPath(const QString& path) {
    if (QFileInfo(path).suffix().compare(QLatin1String("kpro"), Qt::CaseInsensitive) == 0) {
        openProjectFile(path);
        return;
    }
    importPaths({path}, false);
}

void MainWindow::exportMedia() {
    ExportPlan plan;
    QString error;
    if (!session_.prepareExport(session_.activeViewer(), plan, &error)) {
        showError(tr("Cannot export"), error);
        return;
    }
    const bool hasAudio = !plan.audioPath.isEmpty();
    const bool hasBookmarks = !plan.bookmarkedFrames.isEmpty();
    QString name = QFileInfo(session_.sourcePath(session_.activeSourceId())).completeBaseName();
    if (name.isEmpty()) name = tr("export");

    ExportDialog dialog(name, prefs_.ffmpegPath(), hasAudio, hasBookmarks, this);
    if (dialog.exec() != QDialog::Accepted) return;

    auto* job = new ExportJob(std::move(plan), dialog.options(), this);
    auto* progress = new QProgressDialog(tr("Exporting…"), tr("Cancel"), 0, 100, this);
    progress->setWindowModality(Qt::WindowModal);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->setMinimumDuration(0);
    connect(progress, &QProgressDialog::canceled, job, &ExportJob::cancel);
    connect(job, &ExportJob::progress, progress, [progress](int done, int total) {
        progress->setMaximum(total);
        progress->setValue(done);
    });
    connect(job, &ExportJob::finished, this,
            [this, job, progress](bool ok, bool cancelled, const QString& message, int frames) {
                progress->close();
                progress->deleteLater();
                job->deleteLater();
                if (ok) {
                    statusBar()->showMessage(tr("Exported %1").arg(countOf(frames, tr("frame"), tr("frames"))), 8000);
                } else if (cancelled) {
                    statusBar()->showMessage(tr("Export cancelled"), 5000);
                } else {
                    showError(tr("Export failed"), message);
                }
            });
    job->start();
    progress->show();
}

void MainWindow::saveFrameImage() {
    const QImage image = session_.renderFrame(session_.activeViewer(), true);
    if (image.isNull()) {
        showError(tr("Save Frame"), tr("There is no frame to save."));
        return;
    }
    QString path = QFileDialog::getSaveFileName(this, tr("Save Frame as Image"), QString(),
                                                tr("Images (*.png *.jpg *.bmp *.tif)"));
    if (path.isEmpty()) return;
    if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".png");
    if (!image.convertToFormat(QImage::Format_RGB32).save(path)) showError(tr("Save Frame"), tr("Could not write %1").arg(path));
}

void MainWindow::copyFrame() {
    const QImage image = session_.renderFrame(session_.activeViewer(), true);
    if (image.isNull()) return;
    QGuiApplication::clipboard()->setImage(image);
    statusBar()->showMessage(tr("Frame copied"), 3000);
}

void MainWindow::showPreferences() {
    const bool apiWasEnabled = prefs_.apiEnabled();
    const quint16 apiWasPort = prefs_.apiPort();
    PreferencesDialog dialog(prefs_, this);
    if (dialog.exec() != QDialog::Accepted) return;
    applyPreferences();
    // The Client API is restarted only when its own settings changed, so a server started with
    // --api-port (Blender, Maya) survives an unrelated change.
    if (prefs_.apiEnabled() != apiWasEnabled || prefs_.apiPort() != apiWasPort) {
        stopApi();
        if (prefs_.apiEnabled()) {
            QString error;
            if (!startApi(prefs_.apiPort(), &error)) showError(tr("Client API"), error);
        }
    }
}

void MainWindow::applyPreferences() {
    session_.setCacheMegabytes(prefs_.cacheMegabytes());
    audio_->setBufferMs(prefs_.audioBufferMs());
    for (int i = 0; i < 2; ++i) area_->viewer(i)->setScrubPixelsPerFrame(prefs_.scrubPixelsPerFrame());
    if (std::fabs(session_.project().imageSeqFrameRate - prefs_.sequenceFrameRate()) > 1e-9) {
        session_.setImageSequenceFrameRate(prefs_.sequenceFrameRate());
    }
}

// In the active viewer's own frames, 1-based, as the FRAME readout counts them in Global numbering.
void MainWindow::goToFrame() {
    const int count = session_.viewerFrameCount();
    if (count <= 0) return;
    const int offset = session_.viewerOffset(session_.activeViewer());
    bool ok = false;
    const int value = QInputDialog::getInt(this, tr("Go to Frame"), tr("Frame (1–%1):").arg(count),
                                           session_.viewerFrame() + 1, 1, count, 1, &ok);
    if (!ok) return;
    session_.playback().pause();
    session_.playback().seek(value - 1 + offset);
}

void MainWindow::customSpeed() {
    bool ok = false;
    const double value = QInputDialog::getDouble(this, tr("Custom Speed"), tr("Playback speed (1 = normal):"),
                                                 session_.playback().speed(), 0.05, 8.0, 2, &ok);
    if (!ok) return;
    session_.playback().setSpeed(value);
    syncSpeedCombo();
}

void MainWindow::changeSpeed(double factor) {
    session_.playback().setSpeed(session_.playback().speed() * factor);
    syncSpeedCombo();
}

void MainWindow::jumpToAnnotation(int direction) {
    const std::optional<int> target =
        adjacentFrame(session_.annotatedFrames(), session_.playback().frame(), direction);
    if (!target) {
        statusBar()->showMessage(tr("No frame has drawings or notes yet"), 3000);
        return;
    }
    session_.playback().pause();
    session_.playback().seek(*target);
}

void MainWindow::stepSeconds(int seconds) {
    PlaybackController& playback = session_.playback();
    playback.pause();
    playback.step(seconds * std::max(1, static_cast<int>(std::lround(playback.fps()))));
}

// Every command with its key, generated from the menus so it never goes stale. Keys can be changed
// here; they are saved in the preferences and applied at once.
void MainWindow::showShortcuts() {
    const QVector<Command> list = commands();
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Keyboard Shortcuts"));
    dialog.resize(640, 680);

    auto* filter = new QLineEdit(&dialog);
    filter->setPlaceholderText(tr("Search commands or keys…"));
    filter->setClearButtonEnabled(true);

    auto* table = new QTableWidget(static_cast<int>(list.size()), 3, &dialog);
    table->setObjectName(QStringLiteral("shortcuts.table"));
    table->setHorizontalHeaderLabels({tr("Menu"), tr("Command"), tr("Key (click, then press the new key)")});
    table->verticalHeader()->hide();
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setAlternatingRowColors(true);
    QVector<QKeySequenceEdit*> editors;
    for (int row = 0; row < list.size(); ++row) {
        QString text = list[row].action->text();
        text.remove(QLatin1Char('&'));
        table->setItem(row, 0, new QTableWidgetItem(list[row].menu));
        table->setItem(row, 1, new QTableWidgetItem(text));
        auto* edit = new QKeySequenceEdit(list[row].action->shortcut(), table);
        edit->setMaximumSequenceLength(1);
        edit->setFinishingKeyCombinations({QKeyCombination(Qt::Key_Return), QKeyCombination(Qt::Key_Enter),
                                           QKeyCombination(Qt::Key_Tab)});
        edit->setClearButtonEnabled(true);
        table->setCellWidget(row, 2, edit);
        editors.push_back(edit);
    }
    table->resizeColumnsToContents();
    table->horizontalHeader()->setStretchLastSection(true);
    connect(filter, &QLineEdit::textChanged, &dialog, [table, editors](const QString& text) {
        for (int row = 0; row < table->rowCount(); ++row) {
            const QString key = editors[row]->keySequence().toString(QKeySequence::NativeText);
            const bool match = text.isEmpty() || table->item(row, 0)->text().contains(text, Qt::CaseInsensitive) ||
                               table->item(row, 1)->text().contains(text, Qt::CaseInsensitive) ||
                               key.contains(text, Qt::CaseInsensitive);
            table->setRowHidden(row, !match);
        }
    });

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QPushButton* defaults = buttons->addButton(tr("Restore Defaults"), QDialogButtonBox::ResetRole);
    connect(defaults, &QPushButton::clicked, &dialog, [this, &list, editors] {
        for (int row = 0; row < list.size(); ++row) editors[row]->setKeySequence(defaultShortcuts_.value(list[row].id));
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&dialog, table, &list, editors] {
        // One key, one command: list every key that is given twice.
        QHash<QKeySequence, QStringList> users;
        for (int row = 0; row < list.size(); ++row) {
            const QKeySequence key = editors[row]->keySequence();
            // A key that works only inside one panel (Delete in the bookmark list) clashes with nothing.
            if (!key.isEmpty() && list[row].action->shortcutContext() != Qt::WidgetShortcut) {
                users[key].push_back(table->item(row, 1)->text());
            }
        }
        QStringList clashes;
        for (auto it = users.cbegin(); it != users.cend(); ++it) {
            if (it.value().size() > 1) {
                clashes.push_back(QStringLiteral("%1: %2").arg(it.key().toString(QKeySequence::NativeText),
                                                               it.value().join(QStringLiteral(", "))));
            }
        }
        if (!clashes.isEmpty()) {
            QMessageBox::warning(&dialog, tr("Keys used twice"),
                                 tr("Give each key to one command only:\n\n%1").arg(clashes.join(QLatin1Char('\n'))));
            return;
        }
        dialog.accept();
    });

    auto* layout = new QVBoxLayout(&dialog);
    auto* hint = new QLabel(tr("Click a key, press the new key, or use its clear button to remove it."), &dialog);
    hint->setStyleSheet(QStringLiteral("color: #8c8c90;"));
    layout->addWidget(hint);
    layout->addWidget(filter);
    layout->addWidget(table);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) return;

    QHash<QString, QString> changed;
    for (int row = 0; row < list.size(); ++row) {
        const QKeySequence key = editors[row]->keySequence();
        if (key != defaultShortcuts_.value(list[row].id)) {
            changed.insert(list[row].id, key.toString(QKeySequence::PortableText));
        }
    }
    prefs_.setShortcuts(changed);
    applyShortcutOverrides();
    statusBar()->showMessage(tr("Keyboard shortcuts saved"), 4000);
}

void MainWindow::showError(const QString& title, const QString& message) {
    QMessageBox::warning(this, title, message);
}

bool MainWindow::startApi(quint16 port, QString* error) {
    api_ = std::make_unique<ApiServer>(session_);
    if (!api_->listen(port, error)) {
        api_.reset();
        return false;
    }
    statusBar()->showMessage(tr("Client API listening on 127.0.0.1:%1").arg(port), 8000);
    return true;
}

void MainWindow::stopApi() {
    api_.reset();
}

bool MainWindow::splitLayout() const {
    return session_.viewerLayout().startsWith(QLatin1String("split_"));
}

int MainWindow::sessionViewerFor(int widget) const {
    return widget == 0 && splitLayout() ? area_->viewer(0)->pressedSide() : widget;
}

ViewerWidget* MainWindow::widgetFor(int viewer) const {
    return splitLayout() ? area_->viewer(0) : area_->viewer(viewer);
}

void MainWindow::setVolumePercent(int percent) {
    percent = std::clamp(percent, 0, 100);
    audio_->setVolume(percent / 100.0);
    prefs_.setAudioVolume(audio_->volume());
    if (volumeSlider_ && volumeSlider_->value() != percent) {
        const QSignalBlocker blocker(volumeSlider_);
        volumeSlider_->setValue(percent);
    }
    if (volumeLabel_) volumeLabel_->setText(QString::number(percent));
}

void MainWindow::refreshBadges() {
    const auto nameOf = [this](int viewer) {
        const QString id = session_.viewerTargetId(viewer);
        if (const Source* source = session_.project().findSource(id)) return source->name;
        if (const Timeline* timeline = session_.project().findTimeline(id)) return timeline->name;
        return tr("empty: right-click a source, Show as B");
    };
    if (session_.viewerCount() < 2) {
        area_->viewer(0)->setBadges({});
        area_->viewer(1)->setBadges({});
        return;
    }
    const QString a = tr("A  %1").arg(nameOf(0));
    const QString b = tr("B  %1").arg(nameOf(1));
    if (splitLayout()) {
        area_->viewer(0)->setBadges(a, b);
        area_->viewer(1)->setBadges({});
    } else {
        area_->viewer(0)->setBadges(a);
        area_->viewer(1)->setBadges(b);
    }
}

// The notes box keeps typing to itself, except that Space stops playback while the picture moves
// and Esc hands the keys back to the viewer.
bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    // Typing in the notes box never fires a one-key shortcut (I would set the range start), even
    // while the box is read-only during playback. Keys with Ctrl or Alt, F-keys and Esc still work.
    if (watched == noteEdit_ && event->type() == QEvent::ShortcutOverride) {
        auto* key = static_cast<QKeyEvent*>(event);
        const bool command = key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
        const bool functionKey = key->key() >= Qt::Key_F1 && key->key() <= Qt::Key_F35;
        if (!command && !functionKey && key->key() != Qt::Key_Escape) {
            key->accept();
            return true;
        }
    }
    if (watched == noteEdit_ && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            widgetFor(session_.activeViewer())->setFocus(Qt::ShortcutFocusReason);
            return true;
        }
        if (key->key() == Qt::Key_Space && session_.playback().isPlaying()) {
            session_.playback().pause();
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (maybeSave()) {
        prefs_.setWindowLayout(saveGeometry(), saveState());
        prefs_.sync();
        event->accept();
    } else {
        event->ignore();
    }
}

int MainWindow::viewerAt(const QPoint& windowPos) const {
    for (int i = 0; i < 2; ++i) {
        const ViewerWidget* widget = area_->viewer(i);
        if (!widget->isVisible()) continue;
        const QPoint at = widget->mapFrom(this, windowPos);
        if (!widget->rect().contains(at)) continue;
        return i == 0 && splitLayout() ? widget->sideAt(at) : i;
    }
    return -1;
}

// Files can be dropped anywhere (on a viewer they open there); sources from the Project list only
// on a viewer, and they are copied, so the list keeps them.
void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasFormat(QLatin1String(TimelinePanel::kSourceIdsMime))) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
    } else if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void MainWindow::dragMoveEvent(QDragMoveEvent* event) {
    if (!event->mimeData()->hasFormat(QLatin1String(TimelinePanel::kSourceIdsMime))) {
        QMainWindow::dragMoveEvent(event);
        return;
    }
    if (viewerAt(event->position().toPoint()) >= 0) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
    } else {
        event->ignore();
    }
}

void MainWindow::dropEvent(QDropEvent* event) {
    const int target = viewerAt(event->position().toPoint());
    const QLatin1String sourceMime(TimelinePanel::kSourceIdsMime);
    if (event->mimeData()->hasFormat(sourceMime)) {
        const QStringList ids =
            QString::fromUtf8(event->mimeData()->data(sourceMime)).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        if (target < 0 || ids.isEmpty()) {
            event->ignore();
            return;
        }
        event->setDropAction(Qt::CopyAction);
        event->accept();
        showSourceInViewer(ids.first(), target);  // a viewer shows one source
        return;
    }
    QStringList paths;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile()) paths << url.toLocalFile();
    }
    if (paths.isEmpty()) return;
    event->acceptProposedAction();
    if (target >= 0) session_.setActiveViewer(target);  // what is imported opens where it was dropped
    for (const QString& path : paths) {
        if (QFileInfo(path).suffix().compare(QLatin1String("kpro"), Qt::CaseInsensitive) == 0) {
            if (maybeSave()) openProjectFile(path);
            return;  // a project replaces everything; the rest of the drop is ignored
        }
    }
    importPaths(paths, true);
}

}  // namespace fr
