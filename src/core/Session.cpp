#include "core/Session.h"

#include "core/AnnotationCommands.h"
#include "core/Exporter.h"
#include "core/KproFormat.h"
#include "core/StrokeRenderer.h"
#include "media/ImageSequence.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <utility>

namespace fr {
namespace {

QVector<RangeBookmark> sortedRanges(QVector<RangeBookmark> ranges) {
    std::sort(ranges.begin(), ranges.end(), [](const RangeBookmark& a, const RangeBookmark& b) {
        return a.start != b.start ? a.start < b.start : a.end < b.end;
    });
    return ranges;
}

bool isVideoFile(const QString& path) {
    static const QSet<QString> suffixes = {
        QStringLiteral("mp4"), QStringLiteral("mov"), QStringLiteral("mkv"), QStringLiteral("avi"),
        QStringLiteral("webm"), QStringLiteral("mxf"), QStringLiteral("mpg"), QStringLiteral("mpeg"),
        QStringLiteral("m4v"), QStringLiteral("flv"), QStringLiteral("wmv"), QStringLiteral("mts"),
        QStringLiteral("f4v")};
    return suffixes.contains(QFileInfo(path).suffix().toLower());
}

}  // namespace

QString removeStatusText(RemoveStatus status) {
    switch (status) {
        case RemoveStatus::Ok: return {};
        case RemoveStatus::NotFound: return QStringLiteral("Item not found");
        case RemoveStatus::BaseFolder: return QStringLiteral("The base folders cannot be removed");
        case RemoveStatus::NotEmpty: return QStringLiteral("The folder is not empty");
        case RemoveStatus::SourceInTimelines:
            return QStringLiteral("The source is used by a timeline; pass force to remove it anyway");
    }
    return {};
}

Session::Session(QObject* parent) : QObject(parent), project_(Project::createEmpty()) {
    undoStack_.setUndoLimit(200);
    // Playback shows a frame only once every visible viewer has it decoded (see PlaybackController).
    playback_.setFrameGate([this](int playhead) {
        for (int v = 0; v < viewerCount(); ++v) {
            const Slot& slot = slots_[v];
            if (slot.frames && slot.frameCount > 0 && !slot.frames->isReady(slotFrame(v, playhead))) return false;
        }
        return true;
    });
}

Session::~Session() = default;

// --- helpers ---------------------------------------------------------------------------------

int Session::viewerIndex(int viewer) const {
    return std::clamp(viewer < 0 ? activeViewer_ : viewer, 0, 1);
}

int Session::slotFrame(int viewer, int playhead) const {
    const Slot& slot = slots_[viewer];
    if (slot.frameCount <= 0) return 0;
    return std::clamp(playhead - slot.offset, 0, slot.frameCount - 1);
}

Session::ClipPosition Session::clipAt(int viewer, int viewerFrame) const {
    const Slot& slot = slots_[viewer];
    if (slot.clips.isEmpty()) return {};
    int start = 0;
    for (int i = 0; i < slot.clips.size(); ++i) {
        if (viewerFrame < start + slot.clips[i].count) return ClipPosition{i, start};
        start += slot.clips[i].count;
    }
    return ClipPosition{static_cast<int>(slot.clips.size()) - 1, start - slot.clips.last().count};
}

Source* Session::editableLocationSource(int viewer) {
    const std::optional<Location> where = location(viewer);
    return where ? project_.findSource(where->sourceId) : nullptr;
}

void Session::clearSlot(int viewer) {
    Slot& slot = slots_[viewer];
    slot.frames.reset();
    slot.kind.clear();
    slot.id.clear();
    slot.clips.clear();
    slot.frameCount = 0;
}

void Session::resetViewers() {
    clearSlot(0);
    clearSlot(1);
    slots_[1].offset = 0;
    activeViewer_ = 0;
    reconfigurePlayback();
}

void Session::installSlot(int viewer, const QString& kind, const QString& id, const QVector<ClipRef>& clips,
                          std::vector<ClipSequenceSource::Entry> entries, int startFrame) {
    auto sequence = std::make_unique<ClipSequenceSource>(std::move(entries));
    Slot& slot = slots_[viewer];
    slot.frames.reset();  // stop the old decoder before the new one starts
    slot.frames = std::make_unique<AsyncFrameSource>(std::move(sequence), startFrame, nullptr, 0, cacheBytes_);
    slot.kind = kind;
    slot.id = id;
    slot.clips = clips;
    slot.frameCount = slot.frames->frameCount();
    connect(slot.frames.get(), &AsyncFrameSource::frameReady, this, [this, viewer](int index) {
        if (slotFrame(viewer, playback_.frame()) == index) emit frameImageReady(viewer);
    });
}

// Sizes the playhead to the longest viewer. When nothing about the size or the rate changed the
// playhead and range are left alone, so showing something in the second viewer does not rewind.
void Session::reconfigurePlayback() {
    int count = 0;
    double fps = 0.0;
    for (int v = 0; v < viewerCount(); ++v) {
        const Slot& slot = slots_[v];
        if (slot.frameCount > 0) count = std::max(count, slot.offset + slot.frameCount);
    }
    const Slot& active = slots_[activeViewer_];
    if (active.frames) fps = active.frames->fps();
    for (int v = 0; v < viewerCount() && fps <= 0.0; ++v) {
        if (slots_[v].frames) fps = slots_[v].frames->fps();
    }
    if (fps <= 0.0) fps = project_.imageSeqFrameRate;

    if (count == playback_.frameCount() && std::abs(fps - playback_.fps()) < 1e-9) return;
    const bool hadFrames = playback_.frameCount() > 0;
    const int frame = playback_.frame();
    const int rangeStart = playback_.rangeStart();
    const int rangeEnd = playback_.rangeEnd();
    const bool wasWhole = rangeStart == 0 && rangeEnd == playback_.frameCount() - 1;
    playback_.configure(count, fps);
    if (hadFrames && count > 0) {
        // A range that covered everything keeps covering everything when the shown length changes.
        playback_.setRange(wasWhole ? 0 : std::min(rangeStart, count - 1),
                           wasWhole ? count - 1 : std::min(rangeEnd, count - 1));
        playback_.seek(frame);
    }
}

// Remembers where the first viewer's source was left, so it opens there next time.
void Session::storePosition() {
    const Slot& slot = slots_[0];
    if (slot.kind != QLatin1String("source")) return;
    if (Source* source = project_.findSource(slot.id)) {
        source->playbackFrame = slotFrame(0, playback_.frame());
        // The default range (the whole source, or its cut) is stored as "none", so that a cut set
        // later still decides what the source opens with.
        const std::pair<int, int> cut = source->cutRange(slot.frameCount);
        const bool isDefault = playback_.rangeStart() == cut.first && playback_.rangeEnd() == cut.second;
        source->rangeStartFrame = isDefault ? 0 : playback_.rangeStart();
        source->rangeEndFrame = isDefault ? -1 : playback_.rangeEnd();
    }
}

// --- project ---------------------------------------------------------------------------------

void Session::newProject() {
    project_ = Project::createEmpty();
    projectPath_.clear();
    loadWarnings_.clear();
    undoStack_.clear();
    strokeClipboard_.clear();
    resetViewers();
    emit projectReplaced();
    emit viewerLayoutChanged();
}

bool Session::openProject(const QString& filePath, QString* error) {
    LoadResult result = fr::loadProject(filePath);
    if (!result.ok) {
        if (error) *error = result.error;
        return false;
    }
    project_ = std::move(result.project);
    projectPath_ = QFileInfo(filePath).absoluteFilePath();
    loadWarnings_ = result.warnings;
    undoStack_.clear();
    strokeClipboard_.clear();
    resetViewers();
    emit projectReplaced();
    emit viewerLayoutChanged();
    return true;
}

bool Session::saveProject(const QString& filePath, QString* error) {
    const QString target = filePath.isEmpty() ? projectPath_ : filePath;
    if (target.isEmpty()) {
        if (error) *error = QStringLiteral("No project path set");
        return false;
    }
    storePosition();
    if (!fr::saveProject(project_, target, error)) return false;
    projectPath_ = QFileInfo(target).absoluteFilePath();
    return true;
}

// --- importing -------------------------------------------------------------------------------

QString Session::importFile(const QString& filePath, QString* error) {
    const QFileInfo info(filePath);
    if (!info.isFile()) {
        if (error) *error = QStringLiteral("File not found: %1").arg(filePath);
        return {};
    }
    const QString id = project_.addSource(info.absoluteFilePath());
    emit projectChanged();
    return id;
}

QString Session::importSequence(const QString& frameOrPattern, QString* error) {
    const std::optional<SequencePattern> pattern =
        isSequencePath(frameOrPattern) ? parseSequencePath(frameOrPattern) : sequenceFromFile(frameOrPattern);
    if (!pattern) {
        if (error) *error = QStringLiteral("The file name does not end in a frame number: %1").arg(frameOrPattern);
        return {};
    }
    const QVector<int> numbers = scanSequence(*pattern);
    if (numbers.isEmpty()) {
        if (error) *error = QStringLiteral("No frames found for %1").arg(pattern->pattern());
        return {};
    }
    // Named for people: "shot.[0001-0120].png".
    const auto number = [&pattern](int n) {
        return QString::number(n).rightJustified(pattern->digits, QLatin1Char('0'));
    };
    const QString name = QStringLiteral("%1[%2-%3]%4")
                             .arg(pattern->prefix, number(numbers.first()), number(numbers.last()), pattern->suffix);
    const QString id = project_.addSource(pattern->pattern(), name);
    emit projectChanged();
    return id;
}

QStringList Session::importFolder(const QString& folder, QString* error) {
    const QDir directory(folder);
    if (!directory.exists()) {
        if (error) *error = QStringLiteral("Folder not found: %1").arg(folder);
        return {};
    }
    QStringList ids;
    QSet<QString> covered;  // frames that already belong to an imported sequence
    for (const SequenceGroup& group : findSequencesInFolder(folder)) {
        for (const int number : group.numbers) {
            covered.insert(QFileInfo(group.pattern.pathFor(number)).absoluteFilePath());
        }
        const QString id = importSequence(group.pattern.pattern(), nullptr);
        if (!id.isEmpty()) ids << id;
    }
    for (const QString& name : directory.entryList(QDir::Files, QDir::Name)) {
        const QString path = directory.absoluteFilePath(name);
        if (covered.contains(path)) continue;
        if (!isVideoFile(path) && !isImageFile(path)) continue;
        const QString id = importFile(path, nullptr);
        if (!id.isEmpty()) ids << id;
    }
    return ids;
}

// --- viewers ---------------------------------------------------------------------------------

int Session::viewerCount() const {
    return project_.viewerLayout == QLatin1String("single") ? 1 : 2;
}

QString Session::viewerLayout() const {
    return project_.viewerLayout;
}

void Session::setViewerLayout(const QString& layout) {
    const bool known = layout == QLatin1String("horizontal") || layout == QLatin1String("vertical") ||
                       layout == QLatin1String("split_horizontal") || layout == QLatin1String("split_vertical");
    const QString wanted = known ? layout : QStringLiteral("single");
    if (wanted == project_.viewerLayout) return;
    project_.viewerLayout = wanted;
    if (viewerCount() == 1 && activeViewer_ != 0) {
        activeViewer_ = 0;
        emit activeViewerChanged(0);
    }
    reconfigurePlayback();
    emit viewerLayoutChanged();
    emit projectChanged();
}

void Session::setActiveViewer(int viewer) {
    viewer = std::clamp(viewer, 0, viewerCount() - 1);
    if (viewer == activeViewer_) return;
    activeViewer_ = viewer;
    emit activeViewerChanged(viewer);
    emit activeSourceChanged(activeSourceId());
}

bool Session::activateSource(const QString& id, QString* error) {
    Source* source = project_.findSource(id);
    if (!source) {
        if (error) *error = QStringLiteral("Source not found: %1").arg(id);
        return false;
    }
    QString openError;
    std::unique_ptr<FrameSource> fresh =
        openFrameSource(resolveSourcePath(*source, project_, projectPath_), &openError);
    if (!fresh) {
        if (error) *error = openError;
        return false;
    }

    const int viewer = activeViewer_;
    if (viewer == 0) storePosition();
    const int count = fresh->frameCount();
    const int startFrame = viewer == 0 ? std::clamp(source->playbackFrame, 0, std::max(0, count - 1)) : 0;

    std::vector<ClipSequenceSource::Entry> entries;
    entries.push_back(ClipSequenceSource::Entry{std::move(fresh), 0, count});
    installSlot(viewer, QStringLiteral("source"), id, {ClipRef{id, 0, count}}, std::move(entries), startFrame);
    reconfigurePlayback();

    if (viewer == 0) {
        playback_.pause();
        const std::pair<int, int> cut = source->cutRange(count);
        if (source->rangeEndFrame >= 0 && source->rangeEndFrame >= source->rangeStartFrame) {
            playback_.setRange(source->rangeStartFrame, source->rangeEndFrame);
        } else {
            playback_.setRange(cut.first, cut.second);
        }
        playback_.seek(startFrame);
    }
    emit activeSourceChanged(id);
    return true;
}

bool Session::installTimeline(int viewer, const QString& id, QString* error) {
    const Timeline* timeline = project_.findTimeline(id);
    if (!timeline) {
        if (error) *error = QStringLiteral("Timeline not found: %1").arg(id);
        return false;
    }

    std::vector<ClipSequenceSource::Entry> entries;
    QVector<ClipRef> clips;
    QStringList problems;
    for (const Clip& clip : timeline->clips) {
        const Source* source = project_.findSource(clip.sourceId);
        if (!source) {
            problems << QStringLiteral("Missing source %1").arg(clip.sourceId);
            continue;
        }
        QString openError;
        std::unique_ptr<FrameSource> frames =
            openFrameSource(resolveSourcePath(*source, project_, projectPath_), &openError);
        if (!frames) {
            problems << QStringLiteral("%1: %2").arg(source->name, openError);
            continue;
        }
        const std::pair<int, int> cut = source->cutRange(frames->frameCount());
        const int count = cut.second - cut.first + 1;
        if (count <= 0) continue;
        clips.push_back(ClipRef{clip.sourceId, cut.first, count});
        entries.push_back(ClipSequenceSource::Entry{std::move(frames), cut.first, count});
    }
    if (entries.empty()) {
        if (error) {
            *error = problems.isEmpty() ? QStringLiteral("The timeline has no clips") : problems.join(QLatin1Char('\n'));
        }
        return false;
    }

    if (viewer == 0) storePosition();
    installSlot(viewer, QStringLiteral("timeline"), id, clips, std::move(entries), 0);
    reconfigurePlayback();
    return true;
}

bool Session::activateTimeline(const QString& id, QString* error) {
    if (!installTimeline(activeViewer_, id, error)) return false;
    if (activeViewer_ == 0) {
        playback_.pause();
        playback_.resetRange();
        playback_.seek(0);
    }
    emit activeSourceChanged(id);
    return true;
}

void Session::deactivateSource() {
    if (activeViewer_ == 0) storePosition();
    clearSlot(activeViewer_);
    reconfigurePlayback();
    emit activeSourceChanged(QString());
}

void Session::sourceRemoved(const QString& id) {
    bool changed = false;
    for (int v = 0; v < 2; ++v) {
        const bool uses = std::any_of(slots_[v].clips.begin(), slots_[v].clips.end(),
                                      [&id](const ClipRef& clip) { return clip.sourceId == id; });
        if (!uses) continue;
        clearSlot(v);
        changed = true;
    }
    if (!changed) return;
    reconfigurePlayback();
    emit activeSourceChanged(activeSourceId());
}

QVector<ClipRef> Session::viewerClips(int viewer) const {
    return slots_[viewerIndex(viewer)].clips;
}

QString Session::viewerTargetKind(int viewer) const {
    return slots_[viewerIndex(viewer)].kind;
}

QString Session::viewerTargetId(int viewer) const {
    return slots_[viewerIndex(viewer)].id;
}

int Session::viewerFrameCount(int viewer) const {
    return slots_[viewerIndex(viewer)].frameCount;
}

QSize Session::viewerFrameSize(int viewer) const {
    const Slot& slot = slots_[viewerIndex(viewer)];
    return slot.frames ? slot.frames->size() : QSize();
}

int Session::viewerFrame(int viewer) const {
    viewer = viewerIndex(viewer);
    if (slots_[viewer].frameCount <= 0) return 0;
    return slotFrame(viewer, playback_.frame());
}

void Session::setViewerOffset(int viewer, int offset) {
    if (viewer != 1) return;  // the first viewer defines the playhead
    offset = std::clamp(offset, -100000, 100000);
    if (slots_[1].offset == offset) return;
    slots_[1].offset = offset;
    reconfigurePlayback();
    emit viewerOffsetChanged(1);
    emit frameImageReady(1);
}

int Session::viewerOffset(int viewer) const {
    return slots_[viewerIndex(viewer)].offset;
}

std::optional<Location> Session::location(int viewer) const {
    viewer = viewerIndex(viewer);
    const Slot& slot = slots_[viewer];
    if (slot.clips.isEmpty() || slot.frameCount <= 0) return std::nullopt;
    const int frame = slotFrame(viewer, playback_.frame());
    const ClipPosition position = clipAt(viewer, frame);
    const ClipRef& clip = slot.clips[position.index];
    return Location{clip.sourceId, clip.first + std::min(frame - position.start, std::max(0, clip.count - 1))};
}

QString Session::activeSourceId() const {
    const std::optional<Location> where = location(-1);
    return where ? where->sourceId : QString();
}

const Source* Session::locationSource(int viewer) const {
    const std::optional<Location> where = location(viewer);
    return where ? project_.findSource(where->sourceId) : nullptr;
}

QImage Session::currentImage(int viewer) {
    viewer = viewerIndex(viewer);
    Slot& slot = slots_[viewer];
    if (!slot.frames) return {};
    const int frame = slotFrame(viewer, playback_.frame());
    // The cache reads ahead in playing order, round the range when it loops.
    slot.frames->setPlaybackWindow(playback_.rangeStart() - slot.offset, playback_.rangeEnd() - slot.offset,
                                   playback_.loopMode() != LoopMode::Off);
    slot.frames->request(frame, playback_.isForward() ? 1 : -1);
    return slot.frames->nearestFrame(frame);
}

bool Session::currentImageIsExact(int viewer) const {
    viewer = viewerIndex(viewer);
    const Slot& slot = slots_[viewer];
    return slot.frames && slot.frames->isReady(slotFrame(viewer, playback_.frame()));
}

QImage Session::ghostImage(int viewer, int delta) {
    viewer = viewerIndex(viewer);
    Slot& slot = slots_[viewer];
    if (!slot.frames) return {};
    const int frame = slotFrame(viewer, playback_.frame()) + delta;
    if (frame < 0 || frame >= slot.frameCount) return {};
    return slot.frames->exactFrame(frame);
}

QString Session::frameLabel(NumberingMode mode, int viewer) const {
    viewer = viewerIndex(viewer);
    const Slot& slot = slots_[viewer];
    if (slot.frameCount <= 0) return {};
    FrameLabelInput input;
    input.frame = slotFrame(viewer, playback_.frame());
    input.frameCount = slot.frameCount;
    input.rangeStart = std::clamp(playback_.rangeStart() - slot.offset, 0, slot.frameCount - 1);
    input.rangeEnd = std::clamp(playback_.rangeEnd() - slot.offset, input.rangeStart, slot.frameCount - 1);
    input.fps = playback_.fps();
    input.tcOffset = tcOffset(viewer);
    input.imageNumber = slot.frames ? slot.frames->imageNumber(input.frame) : -1;
    return formatFrameLabel(mode, input);
}

int Session::tcOffset(int viewer) const {
    const Source* source = locationSource(viewer);
    if (source && source->tcOffsetOverrideEnabled) return source->tcOffsetOverride;
    return project_.timeControlFrameOffset;
}

// --- drawings --------------------------------------------------------------------------------

QVector<Stroke> Session::strokesToShow(int viewer) const {
    const std::optional<Location> where = location(viewer);
    if (!where) return {};
    const Source* source = project_.findSource(where->sourceId);
    return source ? source->visibleStrokes(where->frame) : QVector<Stroke>();
}

QVector<Stroke> Session::strokesOfFrame(int viewer) const {
    const std::optional<Location> where = location(viewer);
    if (!where) return {};
    const Source* source = project_.findSource(where->sourceId);
    const FrameAnnotation* own = source ? source->annotationAt(where->frame) : nullptr;
    return own ? own->strokes : QVector<Stroke>();
}

bool Session::strokesAreEditable(int viewer) const {
    const std::optional<Location> where = location(viewer);
    if (!where) return false;
    const Source* source = project_.findSource(where->sourceId);
    if (!source) return false;
    const FrameAnnotation* own = source->annotationAt(where->frame);
    return (own && !own->strokes.isEmpty()) || source->visibleStrokes(where->frame).isEmpty();
}

QVector<Stroke> Session::ghostStrokes(int viewer) const {
    const std::optional<Location> where = location(viewer);
    if (!where) return {};
    const Source* source = project_.findSource(where->sourceId);
    if (!source) return {};
    const FrameAnnotation* before = nullptr;
    const FrameAnnotation* after = nullptr;
    for (const FrameAnnotation& annotation : source->annotations) {
        if (annotation.strokes.isEmpty() || annotation.frame == where->frame) continue;
        if (annotation.frame < where->frame && (!before || annotation.frame > before->frame)) before = &annotation;
        if (annotation.frame > where->frame && (!after || annotation.frame < after->frame)) after = &annotation;
    }
    QVector<Stroke> result;
    if (before) result += before->strokes;
    if (after) result += after->strokes;
    return result;
}

void Session::addStroke(const Stroke& stroke) {
    const std::optional<Location> where = location(-1);
    if (!where) return;
    undoStack_.push(new AddStrokeCommand(*this, where->sourceId, where->frame, stroke));
}

void Session::setStrokes(const QVector<Stroke>& strokes, const QString& text) {
    const std::optional<Location> where = location(-1);
    if (!where) return;
    undoStack_.push(new SetStrokesCommand(*this, where->sourceId, where->frame, strokes, text));
}

void Session::clearStrokes() {
    const std::optional<Location> where = location(-1);
    if (!where) return;
    const Source* source = project_.findSource(where->sourceId);
    const FrameAnnotation* annotation = source ? source->annotationAt(where->frame) : nullptr;
    if (!annotation || annotation->strokes.isEmpty()) return;
    undoStack_.push(new ClearStrokesCommand(*this, where->sourceId, where->frame));
}

void Session::erasePath(const QVector<QPointF>& path, double tolerance, double aspect) {
    const QVector<Stroke> strokes = strokesOfFrame(-1);
    if (strokes.isEmpty() || path.isEmpty()) return;
    // A fast sweep gives few mouse samples, so test points along the segments between them too, at
    // steps of at most half the tolerance (distances are in units of the picture's height).
    QVector<QPointF> samples;
    samples.push_back(path.first());
    for (int i = 1; i < path.size(); ++i) {
        const QPointF from = path[i - 1];
        const QPointF to = path[i];
        const double length = std::hypot((to.x() - from.x()) * aspect, to.y() - from.y());
        const int steps = std::max(1, static_cast<int>(std::ceil(length / std::max(1e-6, tolerance * 0.5))));
        for (int k = 1; k <= steps; ++k) samples.push_back(from + (to - from) * (static_cast<double>(k) / steps));
    }
    QVector<Stroke> kept;
    bool removedAny = false;
    for (int i = 0; i < strokes.size(); ++i) {
        bool hit = false;
        for (const QPointF& point : samples) {
            if (distanceToStroke(strokes[i], point, aspect) <= tolerance) {
                hit = true;
                break;
            }
        }
        if (hit) {
            removedAny = true;
        } else {
            kept.push_back(strokes[i]);
        }
    }
    if (removedAny) setStrokes(kept, tr("Erase Drawing"));
}

void Session::moveStroke(int index, const QPointF& delta) {
    QVector<Stroke> strokes = strokesOfFrame(-1);
    if (index < 0 || index >= strokes.size()) return;
    strokes[index] = translatedStroke(strokes[index], delta);
    setStrokes(strokes, tr("Move Drawing"));
}

void Session::deleteStroke(int index) {
    QVector<Stroke> strokes = strokesOfFrame(-1);
    if (index < 0 || index >= strokes.size()) return;
    strokes.removeAt(index);
    setStrokes(strokes, tr("Delete Drawing"));
}

void Session::copyStrokes() {
    const QVector<Stroke> strokes = strokesOfFrame(-1);
    if (!strokes.isEmpty()) strokeClipboard_ = strokes;
}

void Session::cutStrokes() {
    copyStrokes();
    clearStrokes();
}

void Session::pasteStrokes() {
    if (strokeClipboard_.isEmpty() || !location(-1)) return;
    QVector<Stroke> strokes = strokesOfFrame(-1);
    strokes += strokeClipboard_;
    setStrokes(strokes, tr("Paste Drawings"));
}

QString Session::currentNote() const {
    const std::optional<Location> where = location(-1);
    if (!where) return {};
    const Source* source = project_.findSource(where->sourceId);
    const FrameAnnotation* own = source ? source->annotationAt(where->frame) : nullptr;
    return own ? own->note : QString();
}

void Session::setCurrentNote(const QString& text) {
    const std::optional<Location> where = location(-1);
    if (!where) return;
    Source* source = project_.findSource(where->sourceId);
    if (!source) return;
    const FrameAnnotation* existing = source->annotationAt(where->frame);
    if ((existing ? existing->note : QString()) == text) return;
    source->annotationForEdit(where->frame).note = text;
    source->pruneEmptyAnnotations();
    emit annotationsChanged();
}

// --- bookmarks -------------------------------------------------------------------------------

void Session::pushBookmarks(const QString& sourceId, const QVector<int>& frames,
                            const QVector<RangeBookmark>& ranges, const QString& text) {
    undoStack_.push(new SetBookmarksCommand(*this, sourceId, frames, ranges, text));
}

void Session::toggleFrameBookmark() {
    const std::optional<Location> where = location(-1);
    const Source* source = where ? project_.findSource(where->sourceId) : nullptr;
    if (!source) return;
    QVector<int> frames = source->frameBookmarks;
    if (frames.contains(where->frame)) {
        frames.removeAll(where->frame);
    } else {
        frames.push_back(where->frame);
        std::sort(frames.begin(), frames.end());
    }
    pushBookmarks(where->sourceId, frames, source->rangeBookmarks, tr("Toggle Bookmark"));
}

// The playback range, cut to the clip under the playhead, in that source's own frames.
void Session::toggleRangeBookmark() {
    const std::optional<Location> where = location(-1);
    const Source* source = where ? project_.findSource(where->sourceId) : nullptr;
    if (!source || playback_.frameCount() <= 0) return;

    const int viewer = activeViewer_;
    const Slot& slot = slots_[viewer];
    const ClipPosition position = clipAt(viewer, slotFrame(viewer, playback_.frame()));
    if (position.index < 0) return;
    const ClipRef& clip = slot.clips[position.index];
    const int clipStart = position.start + slot.offset;  // the clip's first frame, on the playhead
    const int clipEnd = clipStart + clip.count - 1;
    const int from = std::max(playback_.rangeStart(), clipStart);
    const int to = std::min(playback_.rangeEnd(), clipEnd);
    if (from > to) return;

    const RangeBookmark range{clip.first + (from - clipStart), clip.first + (to - clipStart)};
    QVector<RangeBookmark> ranges = source->rangeBookmarks;
    if (ranges.contains(range)) {
        ranges.removeAll(range);
    } else {
        ranges.push_back(range);
    }
    pushBookmarks(where->sourceId, source->frameBookmarks, sortedRanges(ranges), tr("Toggle Range Bookmark"));
}

void Session::removeFrameBookmark(int sourceFrame) {
    const std::optional<Location> where = location(-1);
    const Source* source = where ? project_.findSource(where->sourceId) : nullptr;
    if (!source || !source->frameBookmarks.contains(sourceFrame)) return;
    QVector<int> frames = source->frameBookmarks;
    frames.removeAll(sourceFrame);
    pushBookmarks(where->sourceId, frames, source->rangeBookmarks, tr("Remove Bookmark"));
}

void Session::removeRangeBookmark(const RangeBookmark& range) {
    const std::optional<Location> where = location(-1);
    const Source* source = where ? project_.findSource(where->sourceId) : nullptr;
    if (!source || !source->rangeBookmarks.contains(range)) return;
    QVector<RangeBookmark> ranges = source->rangeBookmarks;
    ranges.removeAll(range);
    pushBookmarks(where->sourceId, source->frameBookmarks, ranges, tr("Remove Bookmark"));
}

void Session::removeBookmarkEntry(const BookmarkEntry& entry) {
    const Source* source = project_.findSource(entry.sourceId);
    if (!source) return;
    if (entry.isRange) {
        const RangeBookmark range{entry.sourceStart, entry.sourceEnd};
        if (!source->rangeBookmarks.contains(range)) return;
        QVector<RangeBookmark> ranges = source->rangeBookmarks;
        ranges.removeAll(range);
        pushBookmarks(entry.sourceId, source->frameBookmarks, ranges, tr("Remove Bookmark"));
    } else {
        if (!source->frameBookmarks.contains(entry.sourceStart)) return;
        QVector<int> frames = source->frameBookmarks;
        frames.removeAll(entry.sourceStart);
        pushBookmarks(entry.sourceId, frames, source->rangeBookmarks, tr("Remove Bookmark"));
    }
}

void Session::clearBookmarks() {
    const std::optional<Location> where = location(-1);
    const Source* source = where ? project_.findSource(where->sourceId) : nullptr;
    if (!source || (source->frameBookmarks.isEmpty() && source->rangeBookmarks.isEmpty())) return;
    pushBookmarks(where->sourceId, {}, {}, tr("Clear Bookmarks"));
}

bool Session::passesFilter(const Source& source, int frame) const {
    if (bookmarkFilter_ == BookmarkFilter::All) return true;
    const FrameAnnotation* annotation = source.annotationAt(frame);
    if (!annotation) return false;
    return bookmarkFilter_ == BookmarkFilter::WithDrawings ? !annotation->strokes.isEmpty()
                                                           : !annotation->note.isEmpty();
}

// Every bookmark of every clip of a viewer, placed on the viewer's own frames.
QVector<BookmarkEntry> Session::collectBookmarks(int viewer, bool filtered) const {
    viewer = viewerIndex(viewer);
    const Slot& slot = slots_[viewer];
    QVector<BookmarkEntry> entries;
    int start = 0;
    for (const ClipRef& clip : slot.clips) {
        const Source* source = project_.findSource(clip.sourceId);
        const int last = clip.first + clip.count - 1;
        if (source) {
            for (const int frame : source->frameBookmarks) {
                if (frame < clip.first || frame > last) continue;
                if (filtered && !passesFilter(*source, frame)) continue;
                const int where = start + (frame - clip.first) + slot.offset;
                entries.push_back(BookmarkEntry{false, where, where, clip.sourceId, frame, frame});
            }
            for (const RangeBookmark& range : source->rangeBookmarks) {
                const int from = std::max(range.start, clip.first);
                const int to = std::min(range.end, last);
                if (from > to) continue;
                entries.push_back(BookmarkEntry{true, start + (from - clip.first) + slot.offset,
                                                start + (to - clip.first) + slot.offset, clip.sourceId,
                                                range.start, range.end});
            }
        }
        start += clip.count;
    }
    std::stable_sort(entries.begin(), entries.end(),
                     [](const BookmarkEntry& a, const BookmarkEntry& b) { return a.start < b.start; });
    return entries;
}

QVector<int> Session::annotatedFrames(int viewer) const {
    viewer = viewerIndex(viewer);
    const Slot& slot = slots_[viewer];
    QVector<int> frames;
    int start = 0;
    for (const ClipRef& clip : slot.clips) {
        if (const Source* source = project_.findSource(clip.sourceId)) {
            const int last = clip.first + clip.count - 1;
            for (const FrameAnnotation& annotation : source->annotations) {
                if (annotation.isEmpty() || annotation.frame < clip.first || annotation.frame > last) continue;
                frames.push_back(start + (annotation.frame - clip.first) + slot.offset);
            }
        }
        start += clip.count;
    }
    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
    return frames;
}

QVector<BookmarkEntry> Session::bookmarkEntries(int viewer) const {
    return collectBookmarks(viewer, true);
}

void Session::bookmarkMarks(int viewer, QVector<int>& frames, QVector<RangeBookmark>& ranges) const {
    frames.clear();
    ranges.clear();
    for (const BookmarkEntry& entry : collectBookmarks(viewer, false)) {
        if (entry.isRange) {
            ranges.push_back(RangeBookmark{entry.start, entry.end});
        } else {
            frames.push_back(entry.start);
        }
    }
}

void Session::setBookmarkFilter(BookmarkFilter filter) {
    if (filter == bookmarkFilter_) return;
    bookmarkFilter_ = filter;
    emit bookmarksChanged();
}

bool Session::goToAdjacentBookmark(int direction) {
    QVector<int> targets;
    for (const BookmarkEntry& entry : collectBookmarks(-1, true)) targets.push_back(entry.start);
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    const std::optional<int> target = adjacentFrame(targets, playback_.frame(), direction);
    if (!target) return false;
    playback_.pause();
    playback_.seek(*target);
    return true;
}

// --- playback range --------------------------------------------------------------------------

void Session::setRangeStartHere() {
    if (playback_.frameCount() <= 0) return;
    const int frame = playback_.frame();
    playback_.setRange(frame, std::max(frame, playback_.rangeEnd()));
}

void Session::setRangeEndHere() {
    if (playback_.frameCount() <= 0) return;
    const int frame = playback_.frame();
    playback_.setRange(std::min(frame, playback_.rangeStart()), frame);
}

void Session::resetRange() {
    const Slot& slot = slots_[0];
    if (slot.kind == QLatin1String("source") && slot.clips.size() == 1) {
        if (const Source* source = project_.findSource(slot.id)) {
            const std::pair<int, int> cut = source->cutRange(slot.frameCount);
            playback_.setRange(cut.first, cut.second);
            return;
        }
    }
    playback_.resetRange();
}

// --- source properties -----------------------------------------------------------------------

void Session::setCutInHere() {
    const std::optional<Location> where = location(-1);
    Source* source = editableLocationSource();
    if (!source) return;
    source->cutInFrame = where->frame;
    if (source->cutOutFrame >= 0 && source->cutOutFrame < source->cutInFrame) source->cutOutFrame = -1;
    refreshTimelinesUsing(source->id);
    emit projectChanged();
}

void Session::setCutOutHere() {
    const std::optional<Location> where = location(-1);
    Source* source = editableLocationSource();
    if (!source) return;
    source->cutOutFrame = where->frame;
    if (source->cutInFrame > where->frame) source->cutInFrame = where->frame;
    refreshTimelinesUsing(source->id);
    emit projectChanged();
}

void Session::clearCut() {
    Source* source = editableLocationSource();
    if (!source) return;
    source->cutInFrame = 0;
    source->cutOutFrame = -1;
    refreshTimelinesUsing(source->id);
    emit projectChanged();
}

void Session::setTcOffsetOverride(bool enabled, int value) {
    Source* source = editableLocationSource();
    if (!source) return;
    source->tcOffsetOverrideEnabled = enabled;
    source->tcOffsetOverride = value;
    emit projectChanged();
}

void Session::setProjectTcOffset(int value) {
    project_.timeControlFrameOffset = value;
    emit projectChanged();
}

void Session::setAudioDelayMs(double milliseconds) {
    Source* source = editableLocationSource();
    if (!source) return;
    source->audioDelayMs = milliseconds;
    emit projectChanged();
}

void Session::setHoldAnnotations(bool hold) {
    Source* source = editableLocationSource();
    if (!source || source->holdAnnotations == hold) return;
    source->holdAnnotations = hold;
    emit annotationsChanged();
}

void Session::setPanZoomEnabled(bool enabled) {
    Source* source = editableLocationSource();
    if (!source || source->panZoomEnabled == enabled) return;
    source->panZoomEnabled = enabled;
    emit projectChanged();
}

void Session::setSourceView(int viewer, double zoom, double panX, double panY) {
    Source* source = editableLocationSource(viewer);
    if (!source || !source->panZoomEnabled) return;
    source->zoom = zoom;
    source->panX = panX;
    source->panY = panY;
}

void Session::setImageSequenceFrameRate(double fps) {
    if (fps <= 0.0) return;
    project_.imageSeqFrameRate = fps;
    reconfigurePlayback();
    emit projectChanged();
}

// --- the project tree ------------------------------------------------------------------------

bool Session::renameItem(const QString& id, const QString& name) {
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || Project::isBaseFolder(id)) return false;
    if (Folder* folder = project_.findFolder(id)) {
        folder->name = trimmed;
    } else if (Source* source = project_.findSource(id)) {
        source->name = trimmed;
    } else if (Timeline* timeline = project_.findTimeline(id)) {
        timeline->name = trimmed;
    } else {
        return false;
    }
    emit projectChanged();
    return true;
}

QString Session::duplicateSource(const QString& id) {
    const Source* source = project_.findSource(id);
    if (!source) return {};
    Source copy = *source;
    copy.id = project_.newId();
    copy.name = tr("%1 copy").arg(source->name);
    project_.sources.push_back(copy);
    emit projectChanged();
    return copy.id;
}

bool Session::moveItem(const QString& id, const QString& newParentFolderId) {
    if (!project_.findFolder(newParentFolderId) || Project::isBaseFolder(id)) return false;
    if (Folder* folder = project_.findFolder(id)) {
        // Not into itself or into one of its own subfolders.
        QString current = newParentFolderId;
        while (!current.isEmpty()) {
            if (current == id) return false;
            const Folder* up = project_.findFolder(current);
            current = up ? up->parentId : QString();
        }
        folder->parentId = newParentFolderId;
    } else if (Source* source = project_.findSource(id)) {
        source->parentId = newParentFolderId;
    } else if (Timeline* timeline = project_.findTimeline(id)) {
        timeline->parentId = newParentFolderId;
    } else {
        return false;
    }
    emit projectChanged();
    return true;
}

RemoveStatus Session::removeItem(const QString& id, bool force) {
    RemoveStatus status = RemoveStatus::NotFound;
    bool wasSource = false;
    bool wasTimeline = false;
    if (project_.findSource(id)) {
        wasSource = true;
        status = project_.removeSource(id, force);
    } else if (project_.findTimeline(id)) {
        wasTimeline = true;
        status = project_.removeTimeline(id);
    } else if (project_.findFolder(id)) {
        status = project_.removeFolder(id);
    }
    if (status != RemoveStatus::Ok) return status;

    if (wasSource) sourceRemoved(id);
    if (wasTimeline) {
        for (int v = 0; v < 2; ++v) {
            if (slots_[v].kind == QLatin1String("timeline") && slots_[v].id == id) clearSlot(v);
        }
        reconfigurePlayback();
        emit activeSourceChanged(activeSourceId());
    }
    emit projectChanged();
    emit timelinesChanged();
    return status;
}

QString Session::addTimeline(const QString& name) {
    const QString id = project_.addTimeline(name);
    if (!id.isEmpty()) {
        emit projectChanged();
        emit timelinesChanged();
    }
    return id;
}

void Session::setDefaultTimeline(const QString& id) {
    if (!project_.setDefaultTimeline(id)) return;
    emit projectChanged();
    emit timelinesChanged();
}

void Session::addClipToTimeline(const QString& timelineId, const QString& sourceId, int index) {
    const Timeline* timeline = project_.findTimeline(timelineId);
    if (!timeline || !project_.findSource(sourceId)) return;
    QVector<Clip> clips = timeline->clips;
    const int at = (index < 0 || index > clips.size()) ? static_cast<int>(clips.size()) : index;
    clips.insert(at, Clip{sourceId});
    undoStack_.push(new SetTimelineClipsCommand(*this, timelineId, clips, tr("Add Clip")));
}

void Session::removeClipFromTimeline(const QString& timelineId, int index) {
    const Timeline* timeline = project_.findTimeline(timelineId);
    if (!timeline || index < 0 || index >= timeline->clips.size()) return;
    QVector<Clip> clips = timeline->clips;
    clips.removeAt(index);
    undoStack_.push(new SetTimelineClipsCommand(*this, timelineId, clips, tr("Remove Clip")));
}

void Session::moveClipInTimeline(const QString& timelineId, int from, int to) {
    const Timeline* timeline = project_.findTimeline(timelineId);
    if (!timeline || from == to) return;
    const int count = static_cast<int>(timeline->clips.size());
    if (from < 0 || from >= count || to < 0 || to >= count) return;
    QVector<Clip> clips = timeline->clips;
    clips.move(from, to);
    undoStack_.push(new SetTimelineClipsCommand(*this, timelineId, clips, tr("Move Clip")));
}

// A viewer that shows a timeline is rebuilt when the timeline (or a source in it) changes, and
// keeps the playhead where it was.
void Session::notifyTimelineChanged(const QString& timelineId) {
    for (int v = 0; v < 2; ++v) {
        if (slots_[v].kind != QLatin1String("timeline") || slots_[v].id != timelineId) continue;
        // Rebuild the clips behind the viewer and keep the playhead, the range and the playing state
        // where they are (only a viewer that can no longer show the timeline is emptied).
        const int frame = playback_.frame();
        QString error;
        const bool rebuilt = installTimeline(v, timelineId, &error);
        if (!rebuilt) {
            clearSlot(v);
            reconfigurePlayback();
        }
        if (playback_.frameCount() > 0) playback_.seek(std::min(frame, playback_.frameCount() - 1));
        if (v == activeViewer_) emit activeSourceChanged(rebuilt ? timelineId : QString());
    }
    emit timelinesChanged();
}

void Session::refreshTimelinesUsing(const QString& sourceId) {
    for (int v = 0; v < 2; ++v) {
        if (slots_[v].kind != QLatin1String("timeline")) continue;
        const bool uses = std::any_of(slots_[v].clips.begin(), slots_[v].clips.end(),
                                      [&sourceId](const ClipRef& clip) { return clip.sourceId == sourceId; });
        if (!uses) continue;
        const QString timelineId = slots_[v].id;  // by value: the slot may be cleared meanwhile
        notifyTimelineChanged(timelineId);
    }
}

// --- exporting and settings ------------------------------------------------------------------

bool Session::prepareExport(int viewer, ExportPlan& plan, QString* error) const {
    viewer = viewerIndex(viewer);
    const Slot& slot = slots_[viewer];
    if (slot.clips.isEmpty()) {
        if (error) *error = QStringLiteral("Nothing is shown in this viewer.");
        return false;
    }

    std::vector<ClipSequenceSource::Entry> entries;
    for (const ClipRef& clip : slot.clips) {
        const Source* source = project_.findSource(clip.sourceId);
        if (!source) {
            if (error) *error = QStringLiteral("Missing source %1").arg(clip.sourceId);
            return false;
        }
        QString openError;
        std::unique_ptr<FrameSource> frames =
            openFrameSource(resolveSourcePath(*source, project_, projectPath_), &openError);
        if (!frames) {
            if (error) *error = openError;
            return false;
        }
        entries.push_back(ClipSequenceSource::Entry{std::move(frames), clip.first, clip.count});
    }
    auto sequence = std::make_unique<ClipSequenceSource>(std::move(entries));
    plan.frameCount = sequence->frameCount();
    plan.imageNumbers = sequence->frameNumbers();
    plan.fps = (slot.frames && slot.frames->fps() > 0.0) ? slot.frames->fps() : project_.imageSeqFrameRate;
    plan.frames = std::move(sequence);

    plan.strokes.clear();
    plan.bookmarkedFrames.clear();
    int start = 0;
    for (const ClipRef& clip : slot.clips) {
        const Source* source = project_.findSource(clip.sourceId);
        for (int i = 0; i < clip.count; ++i) {
            const QVector<Stroke> strokes = source->visibleStrokes(clip.first + i);
            if (!strokes.isEmpty()) plan.strokes.insert(start + i, strokes);
            if (source->frameBookmarks.contains(clip.first + i)) plan.bookmarkedFrames.push_back(start + i);
        }
        start += clip.count;
    }

    const int last = std::max(0, plan.frameCount - 1);
    plan.rangeStart = std::clamp(playback_.rangeStart() - slot.offset, 0, last);
    plan.rangeEnd = std::clamp(playback_.rangeEnd() - slot.offset, plan.rangeStart, last);

    plan.audioPath.clear();
    plan.audioDelayMs = 0.0;
    if (slot.kind == QLatin1String("source")) {
        if (const Source* source = project_.findSource(slot.id)) {
            if (!isSequencePath(source->path) && !isImageFile(source->path)) {
                plan.audioPath = resolveSourcePath(*source, project_, projectPath_);
                plan.audioDelayMs = source->audioDelayMs;
            }
        }
    }
    return true;
}

QImage Session::renderFrame(int viewer, bool withDrawings) {
    viewer = viewerIndex(viewer);
    Slot& slot = slots_[viewer];
    if (!slot.frames) return {};
    const int frame = slotFrame(viewer, playback_.frame());
    QImage image = slot.frames->exactFrame(frame);
    if (image.isNull()) image = slot.frames->nearestFrame(frame);
    if (image.isNull()) return {};
    return renderExportFrame(image, withDrawings ? strokesToShow(viewer) : QVector<Stroke>(), image.size(),
                             withDrawings);
}

QString Session::sourcePath(const QString& sourceId) const {
    const Source* source = project_.findSource(sourceId);
    return source ? resolveSourcePath(*source, project_, projectPath_) : QString();
}

void Session::setCacheMegabytes(int megabytes) {
    cacheBytes_ = static_cast<qint64>(std::max(64, megabytes)) * 1024 * 1024;
    for (Slot& slot : slots_) {
        if (slot.frames) slot.frames->setBudget(cacheBytes_);
    }
}

}  // namespace fr
