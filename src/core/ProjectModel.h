#pragma once

#include <QColor>
#include <QPointF>
#include <QString>
#include <QVector>

#include <optional>
#include <utility>

namespace fr {

inline const QString kSourcesFolderId = QStringLiteral("sources");
inline const QString kTimelinesFolderId = QStringLiteral("timelines");

struct Stroke {
    enum class Type { Pencil, Arrow, Ellipse, Rectangle, Text };

    Type type = Type::Pencil;
    QColor color = Qt::red;
    double width = 4.0;
    double fillOpacity = 0.0;  // 0: no fill; else the fill of a rectangle, ellipse or text box
    QString text;
    // Normalised to the image: (0,0) is the top-left, (1,1) the bottom-right.
    QVector<QPointF> points;
};

QString strokeTypeName(Stroke::Type type);
std::optional<Stroke::Type> strokeTypeFromName(const QString& name);

// The entry of an ascending list after (direction > 0) or before `frame`, wrapping around at the
// ends; empty for an empty list.
std::optional<int> adjacentFrame(const QVector<int>& sorted, int frame, int direction);

struct FrameAnnotation {
    int frame = 0;
    QString note;
    QString noteLabel;
    QVector<Stroke> strokes;

    bool isEmpty() const { return note.isEmpty() && noteLabel.isEmpty() && strokes.isEmpty(); }
};

struct RangeBookmark {
    int start = 0;
    int end = 0;

    bool operator==(const RangeBookmark&) const = default;
};

struct Folder {
    QString id;
    QString name;
    QString parentId;
    bool expanded = true;
};

struct Source {
    QString id;
    QString name;
    QString path;
    QString relativePath;
    QString parentId;

    int cutInFrame = 0;
    int cutOutFrame = -1;  // -1: no cut-out
    int audioIndex = 0;
    double audioDelayMs = 0.0;
    int tcOffsetOverride = 0;
    bool tcOffsetOverrideEnabled = false;
    double zoom = 1.0;
    bool panZoomEnabled = false;
    bool holdAnnotations = false;  // drawings stay up on the frames after the one they were drawn on
    double panX = 0.0;             // view remembered when panZoomEnabled: pan as fractions of the
    double panY = 0.0;             // viewer's width and height, with `zoom`

    int playbackFrame = 0;
    int rangeStartFrame = 0;
    int rangeEndFrame = -1;  // -1: up to the last frame

    QVector<int> frameBookmarks;
    QVector<RangeBookmark> rangeBookmarks;
    QVector<FrameAnnotation> annotations;

    const FrameAnnotation* annotationAt(int frame) const;
    // The strokes to show on a frame: its own, or with holdAnnotations the ones from the nearest
    // earlier frame that has any.
    QVector<Stroke> visibleStrokes(int frame) const;
    // The trimmed part of the source, {first, last}, for a source of `frameCount` frames. Used when
    // the source is placed on a timeline and as the default playback range.
    std::pair<int, int> cutRange(int frameCount) const;
    FrameAnnotation& annotationForEdit(int frame);  // creates it when missing
    void pruneEmptyAnnotations();

    // Sorts and de-duplicates the bookmarks and drops invalid ones (negative frames, ranges that
    // end before they start). Run after loading a file, which may hold them in any order.
    void normalizeBookmarks();
    // Frame bookmarks plus the start of every range bookmark, sorted and unique: the places the
    // "next / previous bookmark" commands visit.
    QVector<int> bookmarkTargets() const;
    // The bookmark target after (direction > 0) or before the given frame, wrapping around at the
    // ends. Empty when there are none.
    std::optional<int> adjacentBookmark(int frame, int direction) const;
};

struct Clip {
    QString sourceId;
};

struct Timeline {
    QString id;
    QString name;
    QString parentId;
    bool isDefault = false;
    QString audioOverride;
    QVector<Clip> clips;
};

enum class RemoveStatus { Ok, NotFound, BaseFolder, NotEmpty, SourceInTimelines };

struct Project {
    QString metaFormat = QStringLiteral("kpro");
    QString sourceRootDir;
    double imageSeqFrameRate = 24.0;
    int timeControlFrameOffset = 0;
    QString viewerLayout = QStringLiteral("single");

    QVector<Folder> folders;
    QVector<Source> sources;
    QVector<Timeline> timelines;

    static Project createEmpty();
    void ensureBaseFolders();
    static bool isBaseFolder(const QString& id);

    const Folder* findFolder(const QString& id) const;
    Folder* findFolder(const QString& id);
    const Source* findSource(const QString& id) const;
    Source* findSource(const QString& id);
    const Timeline* findTimeline(const QString& id) const;
    Timeline* findTimeline(const QString& id);
    bool idInUse(const QString& id) const;
    QString newId() const;

    // Each add returns the new id, or an empty string when the parent does not exist.
    // An empty parent id means "the matching base folder".
    QString addFolder(const QString& name, const QString& parentId = {});
    QString addSource(const QString& path, const QString& name = {}, const QString& parentId = {});
    QString addTimeline(const QString& name, const QString& parentId = {});

    RemoveStatus removeFolder(const QString& id);
    RemoveStatus removeSource(const QString& id, bool force);
    RemoveStatus removeTimeline(const QString& id);

    const Timeline* defaultTimeline() const;
    bool setDefaultTimeline(const QString& id);
};

}  // namespace fr
