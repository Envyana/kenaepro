#include "core/KproFormat.h"

#include "media/ImageSequence.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointF>
#include <QSaveFile>

#include <utility>

namespace fr {
namespace {

QLatin1String K(const char* key) {
    return QLatin1String(key);
}

QString idFromValue(const QJsonValue& v) {
    if (v.isString()) return v.toString();
    if (v.isDouble()) return QString::number(static_cast<qint64>(v.toDouble()));
    return {};
}

// Stroke points travel as one string, "x,y;x,y;...", in image-normalised coordinates.
QString encodePoints(const QVector<QPointF>& points) {
    QStringList parts;
    parts.reserve(points.size());
    for (const QPointF& p : points) {
        parts << QStringLiteral("%1,%2").arg(p.x(), 0, 'g', 9).arg(p.y(), 0, 'g', 9);
    }
    return parts.join(QLatin1Char(';'));
}

QVector<QPointF> decodePoints(const QString& text) {
    QVector<QPointF> points;
    const QStringList parts = text.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        const QStringList xy = part.split(QLatin1Char(','));
        if (xy.size() != 2) continue;
        bool okX = false;
        bool okY = false;
        const double x = xy[0].toDouble(&okX);
        const double y = xy[1].toDouble(&okY);
        if (okX && okY) points.push_back(QPointF(x, y));
    }
    return points;
}

QJsonObject strokeToJson(const Stroke& s) {
    QJsonObject o;
    o[K("type")] = strokeTypeName(s.type);
    o[K("color")] = s.color.name(QColor::HexArgb);
    o[K("width")] = s.width;
    o[K("fill_opacity")] = s.fillOpacity;
    o[K("text")] = s.text;
    o[K("points")] = encodePoints(s.points);
    return o;
}

std::optional<Stroke> strokeFromJson(const QJsonObject& o) {
    const std::optional<Stroke::Type> type = strokeTypeFromName(o.value(K("type")).toString());
    if (!type) return std::nullopt;
    Stroke s;
    s.type = *type;
    s.color = QColor(o.value(K("color")).toString());
    if (!s.color.isValid()) s.color = Qt::red;
    s.width = o.value(K("width")).toDouble(4.0);
    s.fillOpacity = o.value(K("fill_opacity")).toDouble(0.0);
    s.text = o.value(K("text")).toString();
    s.points = decodePoints(o.value(K("points")).toString());
    return s;
}

QJsonObject annotationToJson(const FrameAnnotation& a) {
    QJsonObject o;
    o[K("frame")] = a.frame;
    o[K("note")] = a.note;
    o[K("note_label")] = a.noteLabel;
    QJsonArray strokes;
    for (const Stroke& s : a.strokes) strokes.append(strokeToJson(s));
    o[K("strokes")] = strokes;
    return o;
}

FrameAnnotation annotationFromJson(const QJsonObject& o, QStringList& warnings) {
    FrameAnnotation a;
    a.frame = o.value(K("frame")).toInt(0);
    a.note = o.value(K("note")).toString();
    a.noteLabel = o.value(K("note_label")).toString();
    for (const QJsonValue& v : o.value(K("strokes")).toArray()) {
        const std::optional<Stroke> s = v.isObject() ? strokeFromJson(v.toObject()) : std::nullopt;
        if (s) {
            a.strokes.push_back(*s);
        } else {
            warnings << QStringLiteral("Skipped a stroke with an unknown type on frame %1").arg(a.frame);
        }
    }
    return a;
}

QJsonObject folderToJson(const Folder& f) {
    QJsonObject o;
    o[K("_id")] = f.id;
    o[K("name")] = f.name;
    o[K("parent_id")] = f.parentId;
    o[K("expanded")] = f.expanded;
    return o;
}

QJsonObject sourceToJson(const Source& s, const QString& projectDir) {
    QString relative = s.relativePath;
    if (!projectDir.isEmpty() && QFileInfo(s.path).isAbsolute()) {
        relative = QDir(projectDir).relativeFilePath(s.path);
    }

    QJsonObject o;
    o[K("_id")] = s.id;
    o[K("name")] = s.name;
    o[K("parent_id")] = s.parentId;
    o[K("path")] = s.path;
    o[K("relative_path")] = relative;
    o[K("cut_in_frame")] = s.cutInFrame;
    o[K("cut_out_frame")] = s.cutOutFrame;
    o[K("audio_index")] = s.audioIndex;
    o[K("audio_delay")] = s.audioDelayMs;
    o[K("tc_offset_override")] = s.tcOffsetOverride;
    o[K("tc_offset_override_enabled")] = s.tcOffsetOverrideEnabled;
    o[K("zoom")] = s.zoom;
    o[K("pan_zoom_enabled")] = s.panZoomEnabled;
    o[K("hold_annotations")] = s.holdAnnotations;
    o[K("pan_x")] = s.panX;
    o[K("pan_y")] = s.panY;
    o[K("playback_frame")] = s.playbackFrame;
    o[K("range_start_frame")] = s.rangeStartFrame;
    o[K("range_end_frame")] = s.rangeEndFrame;

    QJsonArray bookmarks;
    for (int frame : s.frameBookmarks) bookmarks.append(frame);
    o[K("bookmarks")] = bookmarks;

    QJsonArray ranges;
    for (const RangeBookmark& r : s.rangeBookmarks) {
        QJsonObject ro;
        ro[K("range_start")] = r.start;
        ro[K("range_end")] = r.end;
        ranges.append(ro);
    }
    o[K("range_bookmarks")] = ranges;

    QJsonArray frameData;
    for (const FrameAnnotation& a : s.annotations) frameData.append(annotationToJson(a));
    o[K("frame_data")] = frameData;
    return o;
}

Source sourceFromJson(const QJsonObject& o, QStringList& warnings) {
    Source s;
    s.id = idFromValue(o.value(K("_id")));
    s.name = o.value(K("name")).toString();
    s.parentId = idFromValue(o.value(K("parent_id")));
    s.path = o.value(K("path")).toString();
    s.relativePath = o.value(K("relative_path")).toString();
    s.cutInFrame = o.value(K("cut_in_frame")).toInt(0);
    s.cutOutFrame = o.value(K("cut_out_frame")).toInt(-1);
    s.audioIndex = o.value(K("audio_index")).toInt(0);
    s.audioDelayMs = o.value(K("audio_delay")).toDouble(0.0);
    s.tcOffsetOverride = o.value(K("tc_offset_override")).toInt(0);
    s.tcOffsetOverrideEnabled = o.value(K("tc_offset_override_enabled")).toBool(false);
    s.zoom = o.value(K("zoom")).toDouble(1.0);
    s.panZoomEnabled = o.value(K("pan_zoom_enabled")).toBool(false);
    s.holdAnnotations = o.value(K("hold_annotations")).toBool(false);
    s.panX = o.value(K("pan_x")).toDouble(0.0);
    s.panY = o.value(K("pan_y")).toDouble(0.0);
    s.playbackFrame = o.value(K("playback_frame")).toInt(0);
    s.rangeStartFrame = o.value(K("range_start_frame")).toInt(0);
    s.rangeEndFrame = o.value(K("range_end_frame")).toInt(-1);

    for (const QJsonValue& v : o.value(K("bookmarks")).toArray()) s.frameBookmarks.push_back(v.toInt());
    for (const QJsonValue& v : o.value(K("range_bookmarks")).toArray()) {
        if (!v.isObject()) continue;
        const QJsonObject ro = v.toObject();
        s.rangeBookmarks.push_back(
            RangeBookmark{ro.value(K("range_start")).toInt(0), ro.value(K("range_end")).toInt(0)});
    }
    for (const QJsonValue& v : o.value(K("frame_data")).toArray()) {
        if (v.isObject()) s.annotations.push_back(annotationFromJson(v.toObject(), warnings));
    }
    s.normalizeBookmarks();
    return s;
}

QJsonObject timelineToJson(const Timeline& t) {
    QJsonObject o;
    o[K("_id")] = t.id;
    o[K("name")] = t.name;
    o[K("parent_id")] = t.parentId;
    o[K("is_default")] = t.isDefault;
    o[K("audio_override")] = t.audioOverride;
    QJsonArray clips;
    int index = 0;
    for (const Clip& c : t.clips) {
        QJsonObject co;
        co[K("clip_index")] = index++;
        co[K("source_id")] = c.sourceId;
        clips.append(co);
    }
    o[K("clips")] = clips;
    return o;
}

Timeline timelineFromJson(const QJsonObject& o) {
    Timeline t;
    t.id = idFromValue(o.value(K("_id")));
    t.name = o.value(K("name")).toString();
    t.parentId = idFromValue(o.value(K("parent_id")));
    t.isDefault = o.value(K("is_default")).toBool(false);
    t.audioOverride = o.value(K("audio_override")).toString();
    for (const QJsonValue& v : o.value(K("clips")).toArray()) {
        if (v.isObject()) t.clips.push_back(Clip{idFromValue(v.toObject().value(K("source_id")))});
    }
    return t;
}

// Every non-base folder must reach a base folder by following parent ids.
void breakFolderCycles(Project& p, QStringList& warnings) {
    for (Folder& f : p.folders) {
        if (Project::isBaseFolder(f.id)) continue;
        QString current = f.parentId;
        bool reachesBase = false;
        for (int hops = 0; hops <= p.folders.size() && !current.isEmpty(); ++hops) {
            if (Project::isBaseFolder(current)) {
                reachesBase = true;
                break;
            }
            const Folder* up = p.findFolder(current);
            if (!up) break;
            current = up->parentId;
        }
        if (!reachesBase) {
            warnings << QStringLiteral("Folder \"%1\" has a parent loop, moved to SOURCES").arg(f.name);
            f.parentId = kSourcesFolderId;
        }
    }
}

QString expandToken(QString path, const QString& projectDir) {
    if (path.startsWith(kProjectDirToken)) path.replace(0, kProjectDirToken.size(), projectDir);
    return path;
}

// A file exists, or an image sequence has at least one frame on disk.
bool pathExists(const QString& path) {
    if (const std::optional<SequencePattern> sequence = parseSequencePath(path)) {
        return !scanSequence(*sequence).isEmpty();
    }
    return QFileInfo::exists(path);
}

}  // namespace

QJsonObject projectToJson(const Project& project, const QString& projectDir) {
    QJsonObject meta;
    meta[K("format")] = project.metaFormat;

    QJsonObject viewer;
    viewer[K("layout")] = project.viewerLayout;

    QJsonObject proj;
    proj[K("source_root_dir")] = project.sourceRootDir;
    proj[K("image_seq_frame_rate")] = project.imageSeqFrameRate;
    proj[K("time_control_frame_offset")] = project.timeControlFrameOffset;
    proj[K("viewer")] = viewer;

    QJsonArray folders;
    for (const Folder& f : project.folders) folders.append(folderToJson(f));
    QJsonArray sources;
    for (const Source& s : project.sources) sources.append(sourceToJson(s, projectDir));
    QJsonArray timelines;
    for (const Timeline& t : project.timelines) timelines.append(timelineToJson(t));

    QJsonObject root;
    root[K("_meta")] = meta;
    root[K("_project")] = proj;
    root[K("folders")] = folders;
    root[K("sources")] = sources;
    root[K("timelines")] = timelines;
    return root;
}

LoadResult projectFromJson(const QJsonObject& root) {
    LoadResult r;
    if (!root.value(K("_meta")).isObject()) {
        r.error = QStringLiteral("Not a project file: the \"_meta\" object is missing");
        return r;
    }

    Project p;
    p.metaFormat = root.value(K("_meta")).toObject().value(K("format")).toString(p.metaFormat);

    const QJsonObject proj = root.value(K("_project")).toObject();
    p.sourceRootDir = proj.value(K("source_root_dir")).toString();
    p.imageSeqFrameRate = proj.value(K("image_seq_frame_rate")).toDouble(p.imageSeqFrameRate);
    p.timeControlFrameOffset = proj.value(K("time_control_frame_offset")).toInt(0);
    p.viewerLayout = proj.value(K("viewer")).toObject().value(K("layout")).toString(p.viewerLayout);

    const auto accept = [&p, &r](const QString& id, const char* what) {
        if (id.isEmpty()) {
            r.warnings << QStringLiteral("Skipped a %1 without an id").arg(QLatin1String(what));
            return false;
        }
        if (p.idInUse(id)) {
            r.warnings << QStringLiteral("Skipped a %1 with a duplicate id \"%2\"").arg(QLatin1String(what), id);
            return false;
        }
        return true;
    };

    for (const QJsonValue& v : root.value(K("folders")).toArray()) {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        Folder f;
        f.id = idFromValue(o.value(K("_id")));
        f.name = o.value(K("name")).toString();
        f.parentId = idFromValue(o.value(K("parent_id")));
        f.expanded = o.value(K("expanded")).toBool(true);
        if (accept(f.id, "folder")) p.folders.push_back(f);
    }
    p.ensureBaseFolders();

    for (Folder& f : p.folders) {
        if (Project::isBaseFolder(f.id)) continue;
        if (f.parentId.isEmpty() || !p.findFolder(f.parentId)) {
            if (!f.parentId.isEmpty()) {
                r.warnings << QStringLiteral("Folder \"%1\" has an unknown parent, moved to SOURCES").arg(f.name);
            }
            f.parentId = kSourcesFolderId;
        }
    }
    breakFolderCycles(p, r.warnings);

    for (const QJsonValue& v : root.value(K("sources")).toArray()) {
        if (!v.isObject()) continue;
        Source s = sourceFromJson(v.toObject(), r.warnings);
        if (!accept(s.id, "source")) continue;
        if (s.parentId.isEmpty() || !p.findFolder(s.parentId)) s.parentId = kSourcesFolderId;
        p.sources.push_back(s);
    }

    for (const QJsonValue& v : root.value(K("timelines")).toArray()) {
        if (!v.isObject()) continue;
        Timeline t = timelineFromJson(v.toObject());
        if (!accept(t.id, "timeline")) continue;
        if (t.parentId.isEmpty() || !p.findFolder(t.parentId)) t.parentId = kTimelinesFolderId;
        p.timelines.push_back(t);
    }
    if (!p.timelines.isEmpty() && !p.defaultTimeline()) p.timelines.first().isDefault = true;

    r.ok = true;
    r.project = std::move(p);
    return r;
}

LoadResult loadProject(const QString& filePath) {
    LoadResult r;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        r.error = QStringLiteral("Cannot open %1: %2").arg(filePath, file.errorString());
        return r;
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        r.error = QStringLiteral("Invalid JSON at byte %1: %2").arg(parseError.offset).arg(parseError.errorString());
        return r;
    }
    if (!doc.isObject()) {
        r.error = QStringLiteral("Project file must contain a JSON object");
        return r;
    }
    return projectFromJson(doc.object());
}

bool saveProject(const Project& project, const QString& filePath, QString* error) {
    const QFileInfo info(filePath);
    if (!QDir().mkpath(info.absolutePath())) {
        if (error) *error = QStringLiteral("Cannot create folder %1").arg(info.absolutePath());
        return false;
    }
    QSaveFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("Cannot write %1: %2").arg(filePath, file.errorString());
        return false;
    }
    const QJsonDocument doc(projectToJson(project, info.absolutePath()));
    file.write(doc.toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error) *error = QStringLiteral("Cannot finish writing %1: %2").arg(filePath, file.errorString());
        return false;
    }
    return true;
}

QString resolveSourcePath(const Source& source, const Project& project,
                          const QString& projectFilePath) {
    const QString projectDir =
        projectFilePath.isEmpty() ? QString() : QFileInfo(projectFilePath).absolutePath();

    const QString direct = expandToken(source.path, projectDir);
    if (!direct.isEmpty() && pathExists(direct)) return direct;

    if (!source.relativePath.isEmpty()) {
        const QString relative = expandToken(source.relativePath, projectDir);
        if (QFileInfo(relative).isAbsolute() && pathExists(relative)) return relative;
        if (!projectDir.isEmpty()) {
            const QString candidate = QDir(projectDir).absoluteFilePath(relative);
            if (pathExists(candidate)) return candidate;
        }
        if (!project.sourceRootDir.isEmpty()) {
            const QString candidate =
                QDir(expandToken(project.sourceRootDir, projectDir)).absoluteFilePath(relative);
            if (pathExists(candidate)) return candidate;
        }
    }
    return direct;
}

}  // namespace fr
