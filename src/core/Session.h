#pragma once

#include "core/FrameNumbering.h"
#include "core/PlaybackController.h"
#include "core/ProjectModel.h"
#include "media/AsyncFrameSource.h"
#include "media/ClipSequenceSource.h"
#include "media/FrameSource.h"

#include <QImage>
#include <QObject>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QUndoStack>
#include <QVector>

#include <memory>
#include <optional>
#include <vector>

namespace fr {

struct ExportPlan;  // core/Exporter.h

// A run of frames of one source: frames [first, first + count).
struct ClipRef {
    QString sourceId;
    int first = 0;
    int count = 0;
};

// One frame of one source.
struct Location {
    QString sourceId;
    int frame = 0;
};

// A bookmark as the bookmarks panel lists it. `start` and `end` are positions in what the viewer
// shows (a timeline's own frame numbers); the source fields are the same frames in the source.
struct BookmarkEntry {
    bool isRange = false;
    int start = 0;
    int end = 0;
    QString sourceId;
    int sourceStart = 0;
    int sourceEnd = 0;
};

enum class BookmarkFilter { All, WithDrawings, WithNotes };

QString removeStatusText(RemoveStatus status);

// The state of one open project: the data model, what the viewers show, and playback. The GUI
// and the Client API both drive the application through this class.
//
// Up to two viewers can be open (the layout is "single", "horizontal" or "vertical"). Each shows
// one source or one timeline. They share one playhead; the second viewer can be shifted against it
// with an offset so that two takes line up. Editing (drawing, bookmarks, notes) always applies to
// the *active* viewer, at the playhead, in the source frame that is under it.
class Session : public QObject {
    Q_OBJECT
public:
    explicit Session(QObject* parent = nullptr);
    ~Session() override;

    // --- project ---------------------------------------------------------------------------
    Project& project() { return project_; }
    const Project& project() const { return project_; }
    PlaybackController& playback() { return playback_; }
    const QString& projectPath() const { return projectPath_; }
    const QStringList& loadWarnings() const { return loadWarnings_; }
    QUndoStack& undoStack() { return undoStack_; }

    void newProject();
    bool openProject(const QString& filePath, QString* error = nullptr);
    // An empty path saves to the current project path.
    bool saveProject(const QString& filePath = {}, QString* error = nullptr);

    // --- importing -------------------------------------------------------------------------
    // One file as one source. Returns the new source id, or an empty string.
    QString importFile(const QString& filePath, QString* error = nullptr);
    // An image sequence, from any numbered frame of it or from its "%04d" pattern.
    QString importSequence(const QString& frameOrPattern, QString* error = nullptr);
    // Every video, every image sequence and every other image in a folder. Returns the new ids.
    QStringList importFolder(const QString& folder, QString* error = nullptr);

    // --- what the viewers show -------------------------------------------------------------
    int viewerCount() const;  // 1 or 2, from the layout
    QString viewerLayout() const;
    // "single", "horizontal" (side by side), "vertical" (stacked), "split_horizontal" (A | B in one
    // viewer) or "split_vertical" (A over B in one viewer). Anything else is "single".
    void setViewerLayout(const QString& layout);
    int activeViewer() const { return activeViewer_; }
    void setActiveViewer(int viewer);

    // Show something in the active viewer. A source opens at its saved frame and range.
    bool activateSource(const QString& id, QString* error = nullptr);
    // The clips of a timeline play back to back, each trimmed to its source's cut range.
    bool activateTimeline(const QString& id, QString* error = nullptr);
    void deactivateSource();               // empties the active viewer
    void sourceRemoved(const QString& id);  // empties every viewer that shows this source

    QString viewerTargetKind(int viewer = -1) const;  // "source", "timeline" or empty
    QString viewerTargetId(int viewer = -1) const;
    int viewerFrameCount(int viewer = -1) const;
    QSize viewerFrameSize(int viewer = -1) const;  // picture size of what a viewer shows; empty when nothing
    // The viewer's own frame under the playhead (the playhead minus the viewer's offset), clamped.
    int viewerFrame(int viewer = -1) const;
    // The second viewer's shift against the playhead, in frames. The first viewer has none.
    void setViewerOffset(int viewer, int offset);
    int viewerOffset(int viewer) const;

    // The source frame under the playhead of a viewer (-1: the active one).
    std::optional<Location> location(int viewer = -1) const;
    // The runs of source frames a viewer shows, in order (one for a source, the clips of a timeline).
    QVector<ClipRef> viewerClips(int viewer = -1) const;
    QString activeSourceId() const;  // the source under the active viewer's playhead
    const Source* locationSource(int viewer = -1) const;

    // The picture for a viewer. Never waits for a decoder: when the exact frame is not decoded yet
    // this is the closest one that is, and frameImageReady() fires once the exact frame arrives.
    QImage currentImage(int viewer = -1);
    // True when currentImage() is the exact frame under the playhead, not a nearby stand-in.
    bool currentImageIsExact(int viewer = -1) const;
    // The decoded frame `delta` frames away from the playhead, if it is in the cache (for ghosting).
    QImage ghostImage(int viewer, int delta);
    // The frame counter text of a viewer, in the chosen numbering.
    QString frameLabel(NumberingMode mode, int viewer = -1) const;
    int tcOffset(int viewer = -1) const;  // frames added to the global number and the timecode

    // --- drawings --------------------------------------------------------------------------
    QVector<Stroke> strokesToShow(int viewer = -1) const;  // with "hold annotations" applied
    QVector<Stroke> strokesOfFrame(int viewer = -1) const;  // only those drawn on this very frame
    bool strokesAreEditable(int viewer = -1) const;  // what is shown is this frame's own
    QVector<Stroke> ghostStrokes(int viewer = -1) const;  // from the nearest annotated frames around

    // All of these are undoable and do nothing when the viewer is empty.
    void addStroke(const Stroke& stroke);
    void setStrokes(const QVector<Stroke>& strokes, const QString& text);
    void clearStrokes();
    // Removes every stroke that the path of an eraser passes within `tolerance` of. Points are
    // normalised to the picture; `aspect` is picture width / height.
    void erasePath(const QVector<QPointF>& path, double tolerance, double aspect);
    void moveStroke(int index, const QPointF& delta);
    void deleteStroke(int index);
    void copyStrokes();
    void cutStrokes();
    void pasteStrokes();
    bool hasStrokeClipboard() const { return !strokeClipboard_.isEmpty(); }

    // The note of the frame under the playhead. Not undoable (the text box has its own undo).
    QString currentNote() const;
    void setCurrentNote(const QString& text);

    // --- bookmarks (of the source under the playhead; frames are 0-based) --------------------
    // All are undoable and do nothing without a source.
    void toggleFrameBookmark();   // at the playhead
    void toggleRangeBookmark();   // the playback range, cut to the clip under the playhead
    void removeFrameBookmark(int sourceFrame);
    void removeRangeBookmark(const RangeBookmark& range);
    void removeBookmarkEntry(const BookmarkEntry& entry);
    void clearBookmarks();
    // Moves to the next (+1) or previous (-1) bookmark, wrapping around. False when there is none.
    bool goToAdjacentBookmark(int direction);
    // Marks for the slider, in playhead frames (the viewer's offset added), unfiltered.
    void bookmarkMarks(int viewer, QVector<int>& frames, QVector<RangeBookmark>& ranges) const;
    // Frames that have drawings or a note, in playhead frames (the viewer's offset added), sorted.
    QVector<int> annotatedFrames(int viewer = -1) const;
    // What the bookmarks panel lists, in order, with the filter applied to frame bookmarks.
    QVector<BookmarkEntry> bookmarkEntries(int viewer = -1) const;
    void setBookmarkFilter(BookmarkFilter filter);
    BookmarkFilter bookmarkFilter() const { return bookmarkFilter_; }

    // --- playback range --------------------------------------------------------------------
    void setRangeStartHere();
    void setRangeEndHere();
    void resetRange();  // the whole of what is shown, or a source's cut range

    // --- properties of the source under the playhead (not undoable) ---------------------------
    void setCutInHere();
    void setCutOutHere();
    void clearCut();
    void setTcOffsetOverride(bool enabled, int value);
    void setProjectTcOffset(int value);
    void setAudioDelayMs(double milliseconds);
    void setHoldAnnotations(bool hold);
    void setPanZoomEnabled(bool enabled);
    // Remembered only while the source has panZoomEnabled.
    void setSourceView(int viewer, double zoom, double panX, double panY);
    void setImageSequenceFrameRate(double fps);

    // --- the project tree ------------------------------------------------------------------
    bool renameItem(const QString& id, const QString& name);
    QString duplicateSource(const QString& id);
    bool moveItem(const QString& id, const QString& newParentFolderId);
    // For a source that timelines use, pass force = true to remove its clips as well.
    RemoveStatus removeItem(const QString& id, bool force);
    QString addTimeline(const QString& name);
    void setDefaultTimeline(const QString& id);
    // Timeline editing, undoable. `index` -1 appends.
    void addClipToTimeline(const QString& timelineId, const QString& sourceId, int index = -1);
    void removeClipFromTimeline(const QString& timelineId, int index);
    void moveClipInTimeline(const QString& timelineId, int from, int to);

    // --- exporting, snapshots, settings ----------------------------------------------------
    // Opens independent decoders for what a viewer shows and gathers the drawings.
    bool prepareExport(int viewer, ExportPlan& plan, QString* error) const;
    // The current frame as a picture (with its drawings burned in if asked), for saving or copying.
    QImage renderFrame(int viewer, bool withDrawings);
    QString sourcePath(const QString& sourceId) const;  // where the media file is, resolved
    void setCacheMegabytes(int megabytes);  // frame cache per viewer, applied at once

    // Called by the undo commands and by code that edits the project directly.
    void notifyProjectChanged() { emit projectChanged(); }
    void notifyAnnotationsChanged() { emit annotationsChanged(); }
    void notifyBookmarksChanged() { emit bookmarksChanged(); }
    void notifyTimelineChanged(const QString& timelineId);

signals:
    void projectReplaced();                       // new/open: everything changed
    void projectChanged();                        // an edit of the project or its tree
    void activeSourceChanged(const QString& id);  // what the active viewer shows changed
    void annotationsChanged();                    // drawing, undo, redo, clear, notes, hold
    void frameImageReady(int viewer);             // the exact picture of a viewer's frame arrived
    void bookmarksChanged();
    void timelinesChanged();
    void viewerLayoutChanged();
    void activeViewerChanged(int viewer);
    void viewerOffsetChanged(int viewer);

private:
    struct Slot {
        QString kind;  // "", "source" or "timeline"
        QString id;
        QVector<ClipRef> clips;
        std::unique_ptr<AsyncFrameSource> frames;
        int frameCount = 0;
        int offset = 0;  // the viewer's frame = playhead - offset
    };
    struct ClipPosition {
        int index = -1;  // which clip
        int start = 0;   // the viewer's frame at which it starts
    };

    int viewerIndex(int viewer) const;
    int slotFrame(int viewer, int playhead) const;
    ClipPosition clipAt(int viewer, int viewerFrame) const;
    Source* editableLocationSource(int viewer = -1);
    void installSlot(int viewer, const QString& kind, const QString& id, const QVector<ClipRef>& clips,
                     std::vector<ClipSequenceSource::Entry> entries, int startFrame);
    void clearSlot(int viewer);
    // Builds the clips of a timeline behind a viewer. Leaves the playhead and the range alone.
    bool installTimeline(int viewer, const QString& id, QString* error);
    void resetViewers();
    void reconfigurePlayback();
    void storePosition();
    void pushBookmarks(const QString& sourceId, const QVector<int>& frames,
                       const QVector<RangeBookmark>& ranges, const QString& text);
    QVector<BookmarkEntry> collectBookmarks(int viewer, bool filtered) const;
    bool passesFilter(const Source& source, int frame) const;
    void refreshTimelinesUsing(const QString& sourceId);

    Project project_;
    PlaybackController playback_;
    QString projectPath_;
    QStringList loadWarnings_;
    QUndoStack undoStack_;
    Slot slots_[2];
    int activeViewer_ = 0;
    qint64 cacheBytes_ = 256LL * 1024 * 1024;
    BookmarkFilter bookmarkFilter_ = BookmarkFilter::All;
    QVector<Stroke> strokeClipboard_;
};

}  // namespace fr
