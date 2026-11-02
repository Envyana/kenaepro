#pragma once

#include "core/ProjectModel.h"

#include <QString>
#include <QUndoCommand>
#include <QVector>

namespace fr {

class Session;

// Undoable edits of what is attached to frames: the strokes drawn on one frame, and a source's
// bookmarks. They address the source by id (and the frame by number) rather than by pointer, so
// they stay valid while other edits happen, and they do nothing when the source has since been
// removed.

class AddStrokeCommand : public QUndoCommand {
public:
    AddStrokeCommand(Session& session, QString sourceId, int frame, Stroke stroke);
    void undo() override;
    void redo() override;

private:
    Session& session_;
    QString sourceId_;
    int frame_;
    Stroke stroke_;
};

class ClearStrokesCommand : public QUndoCommand {
public:
    ClearStrokesCommand(Session& session, QString sourceId, int frame);
    void undo() override;
    void redo() override;

private:
    Session& session_;
    QString sourceId_;
    int frame_;
    QVector<Stroke> removed_;
};

// Replaces a source's whole set of bookmarks. Toggling, adding, removing and clearing all become
// "the bookmarks were X, now they are Y", which undoes exactly.
class SetBookmarksCommand : public QUndoCommand {
public:
    SetBookmarksCommand(Session& session, QString sourceId, QVector<int> frames,
                        QVector<RangeBookmark> ranges, const QString& text);
    void undo() override;
    void redo() override;

private:
    Session& session_;
    QString sourceId_;
    QVector<int> afterFrames_;
    QVector<RangeBookmark> afterRanges_;
    QVector<int> beforeFrames_;
    QVector<RangeBookmark> beforeRanges_;
    bool captured_ = false;  // the "before" state is read from the source on the first redo
};

// Replaces all the strokes of one frame: erasing, moving, deleting, pasting.
class SetStrokesCommand : public QUndoCommand {
public:
    SetStrokesCommand(Session& session, QString sourceId, int frame, QVector<Stroke> strokes,
                      const QString& text);
    void undo() override;
    void redo() override;

private:
    Session& session_;
    QString sourceId_;
    int frame_;
    QVector<Stroke> after_;
    QVector<Stroke> before_;
    bool captured_ = false;
};

// Replaces the clips of a timeline: adding, removing and reordering them.
class SetTimelineClipsCommand : public QUndoCommand {
public:
    SetTimelineClipsCommand(Session& session, QString timelineId, QVector<Clip> clips, const QString& text);
    void undo() override;
    void redo() override;

private:
    Session& session_;
    QString timelineId_;
    QVector<Clip> after_;
    QVector<Clip> before_;
    bool captured_ = false;
};

}  // namespace fr
