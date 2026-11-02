#include "core/AnnotationCommands.h"

#include "core/Session.h"

#include <QCoreApplication>

#include <utility>

namespace fr {

AddStrokeCommand::AddStrokeCommand(Session& session, QString sourceId, int frame, Stroke stroke)
    : session_(session), sourceId_(std::move(sourceId)), frame_(frame), stroke_(std::move(stroke)) {
    setText(QCoreApplication::translate("AddStrokeCommand", "Add Drawing"));
}

void AddStrokeCommand::redo() {
    Source* source = session_.project().findSource(sourceId_);
    if (!source) return;
    source->annotationForEdit(frame_).strokes.push_back(stroke_);
    session_.notifyAnnotationsChanged();
}

void AddStrokeCommand::undo() {
    Source* source = session_.project().findSource(sourceId_);
    if (!source) return;
    FrameAnnotation& annotation = source->annotationForEdit(frame_);
    if (!annotation.strokes.isEmpty()) annotation.strokes.pop_back();
    source->pruneEmptyAnnotations();  // keeps the annotation when it still has a note
    session_.notifyAnnotationsChanged();
}

ClearStrokesCommand::ClearStrokesCommand(Session& session, QString sourceId, int frame)
    : session_(session), sourceId_(std::move(sourceId)), frame_(frame) {
    setText(QCoreApplication::translate("ClearStrokesCommand", "Clear Drawings"));
}

void ClearStrokesCommand::redo() {
    Source* source = session_.project().findSource(sourceId_);
    if (!source) return;
    FrameAnnotation& annotation = source->annotationForEdit(frame_);
    removed_ = annotation.strokes;
    annotation.strokes.clear();
    source->pruneEmptyAnnotations();
    session_.notifyAnnotationsChanged();
}

void ClearStrokesCommand::undo() {
    Source* source = session_.project().findSource(sourceId_);
    if (!source) return;
    source->annotationForEdit(frame_).strokes = removed_;
    session_.notifyAnnotationsChanged();
}

SetBookmarksCommand::SetBookmarksCommand(Session& session, QString sourceId, QVector<int> frames,
                                         QVector<RangeBookmark> ranges, const QString& text)
    : session_(session),
      sourceId_(std::move(sourceId)),
      afterFrames_(std::move(frames)),
      afterRanges_(std::move(ranges)) {
    setText(text);
}

void SetBookmarksCommand::redo() {
    Source* source = session_.project().findSource(sourceId_);
    if (!source) return;
    if (!captured_) {
        beforeFrames_ = source->frameBookmarks;
        beforeRanges_ = source->rangeBookmarks;
        captured_ = true;
    }
    source->frameBookmarks = afterFrames_;
    source->rangeBookmarks = afterRanges_;
    session_.notifyBookmarksChanged();
}

void SetBookmarksCommand::undo() {
    Source* source = session_.project().findSource(sourceId_);
    if (!source) return;
    source->frameBookmarks = beforeFrames_;
    source->rangeBookmarks = beforeRanges_;
    session_.notifyBookmarksChanged();
}

SetStrokesCommand::SetStrokesCommand(Session& session, QString sourceId, int frame,
                                     QVector<Stroke> strokes, const QString& text)
    : session_(session), sourceId_(std::move(sourceId)), frame_(frame), after_(std::move(strokes)) {
    setText(text);
}

void SetStrokesCommand::redo() {
    Source* source = session_.project().findSource(sourceId_);
    if (!source) return;
    FrameAnnotation& annotation = source->annotationForEdit(frame_);
    if (!captured_) {
        before_ = annotation.strokes;
        captured_ = true;
    }
    annotation.strokes = after_;
    source->pruneEmptyAnnotations();
    session_.notifyAnnotationsChanged();
}

void SetStrokesCommand::undo() {
    Source* source = session_.project().findSource(sourceId_);
    if (!source) return;
    source->annotationForEdit(frame_).strokes = before_;
    source->pruneEmptyAnnotations();
    session_.notifyAnnotationsChanged();
}

SetTimelineClipsCommand::SetTimelineClipsCommand(Session& session, QString timelineId,
                                                 QVector<Clip> clips, const QString& text)
    : session_(session), timelineId_(std::move(timelineId)), after_(std::move(clips)) {
    setText(text);
}

void SetTimelineClipsCommand::redo() {
    Timeline* timeline = session_.project().findTimeline(timelineId_);
    if (!timeline) return;
    if (!captured_) {
        before_ = timeline->clips;
        captured_ = true;
    }
    timeline->clips = after_;
    session_.notifyTimelineChanged(timelineId_);
}

void SetTimelineClipsCommand::undo() {
    Timeline* timeline = session_.project().findTimeline(timelineId_);
    if (!timeline) return;
    timeline->clips = before_;
    session_.notifyTimelineChanged(timelineId_);
}

}  // namespace fr
