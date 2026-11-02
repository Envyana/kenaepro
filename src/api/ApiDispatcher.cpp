#include "api/ApiDispatcher.h"

#include "core/PlaybackController.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QStandardPaths>
#include <QStringList>

namespace fr {
namespace {

QLatin1String K(const char* key) {
    return QLatin1String(key);
}

bool requireString(const QJsonObject& req, const char* key, QString& out, QString& error) {
    const QJsonValue v = req.value(K(key));
    if (!v.isString() || v.toString().isEmpty()) {
        error = QStringLiteral("\"%1\" must be a non-empty string").arg(QLatin1String(key));
        return false;
    }
    out = v.toString();
    return true;
}

bool requireInt(const QJsonObject& req, const char* key, int& out, QString& error) {
    const QJsonValue v = req.value(K(key));
    if (!v.isDouble()) {
        error = QStringLiteral("\"%1\" must be a number").arg(QLatin1String(key));
        return false;
    }
    out = v.toInt();
    return true;
}

QString optString(const QJsonObject& req, const char* key) {
    return req.value(K(key)).toString();
}

bool optBool(const QJsonObject& req, const char* key, bool fallback) {
    const QJsonValue v = req.value(K(key));
    return v.isBool() ? v.toBool() : fallback;
}

QJsonObject failure(const QString& message) {
    QJsonObject o;
    o[K("success")] = false;
    o[K("msg")] = message;
    return o;
}

QString removeError(RemoveStatus status) {
    switch (status) {
        case RemoveStatus::NotFound: return QStringLiteral("Object not found");
        case RemoveStatus::BaseFolder: return QStringLiteral("The base folders cannot be removed");
        case RemoveStatus::NotEmpty: return QStringLiteral("The folder is not empty");
        case RemoveStatus::SourceInTimelines:
            return QStringLiteral("The source is used by a timeline; pass \"force\": true to remove it anyway");
        case RemoveStatus::Ok: break;
    }
    return {};
}

// Commands of the original API that this skeleton recognises but has not implemented yet.
const QStringList& plannedCommands() {
    static const QStringList list = {"is_autoplay", "replace_source", "activate_window", "import_project",
                                     "update", "queue_export", "query_export"};
    return list;
}

}  // namespace

ApiDispatcher::ApiDispatcher(Session& session) : session_(session) {
    registerHandlers();
}

QJsonObject ApiDispatcher::handle(const QJsonObject& request) {
    const QString cmd = request.value(K("cmd")).toString();
    if (cmd.isEmpty()) return failure(QStringLiteral("Missing \"cmd\" field"));

    if (cmd != QLatin1String("echo") && cmd != QLatin1String("initialize") && !initialized_) {
        return failure(QStringLiteral("Not initialized: send \"initialize\" first"));
    }

    const auto it = handlers_.constFind(cmd);
    if (it == handlers_.constEnd()) {
        if (plannedCommands().contains(cmd)) {
            return failure(QStringLiteral("Command not implemented yet: %1").arg(cmd));
        }
        return failure(QStringLiteral("Unknown command: %1").arg(cmd));
    }

    QJsonObject result;
    QString error;
    if (!(*it)(request, result, error)) {
        return failure(error.isEmpty() ? QStringLiteral("Command failed") : error);
    }
    QJsonObject response;
    response[K("success")] = true;
    response[K("result")] = result;
    return response;
}

void ApiDispatcher::registerHandlers() {
    const auto reg = [this](const char* name, Handler handler) {
        handlers_.insert(QString::fromLatin1(name), std::move(handler));
    };

    reg("echo", [](const QJsonObject& req, QJsonObject& out, QString&) {
        out = req;
        out.remove(K("cmd"));
        return true;
    });

    reg("initialize", [this](const QJsonObject& req, QJsonObject& out, QString& err) {
        if (initialized_) {
            err = QStringLiteral("Already initialized");
            return false;
        }
        int version = 0;
        if (!requireInt(req, "api_version", version, err)) return false;
        if (version < 1 || version > kApiVersion) {
            err = QStringLiteral("Unsupported api_version %1 (supported: 1..%2)").arg(version).arg(kApiVersion);
            return false;
        }
        initialized_ = true;
        out[K("app_name")] = QCoreApplication::applicationName();
        out[K("app_dir")] = QCoreApplication::applicationDirPath();
        out[K("user_dir")] = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        out[K("temp_dir")] = QDir::tempPath();
        return true;
    });

    // --- project -----------------------------------------------------------------------
    reg("get_project_path", [this](const QJsonObject&, QJsonObject& out, QString&) {
        out[K("project_path")] = session_.projectPath();
        return true;
    });

    reg("new_project", [this](const QJsonObject&, QJsonObject&, QString&) {
        session_.newProject();
        return true;
    });

    reg("open_project", [this](const QJsonObject& req, QJsonObject& out, QString& err) {
        QString path;
        if (!requireString(req, "project_path", path, err)) return false;
        if (!QFileInfo::exists(path)) {
            err = QStringLiteral("Project file does not exist: %1").arg(path);
            return false;
        }
        if (!session_.openProject(path, &err)) return false;
        out[K("project_path")] = session_.projectPath();
        return true;
    });

    reg("save_project", [this](const QJsonObject& req, QJsonObject& out, QString& err) {
        if (!session_.saveProject(optString(req, "project_path"), &err)) return false;
        out[K("project_path")] = session_.projectPath();
        return true;
    });

    reg("import_file", [this](const QJsonObject& req, QJsonObject& out, QString& err) {
        QString path;
        if (!requireString(req, "file_path", path, err)) return false;
        // "sequence": true imports every numbered image next to `file_path` as one source.
        const QString id = optBool(req, "sequence", false) ? session_.importSequence(path, &err)
                                                           : session_.importFile(path, &err);
        if (id.isEmpty()) return false;
        if (optBool(req, "activate", false) && !session_.activateSource(id, &err)) return false;
        out[K("source_id")] = id;
        return true;
    });

    // --- project contents --------------------------------------------------------------
    reg("get_folders", [this](const QJsonObject&, QJsonObject& out, QString&) {
        QJsonArray list;
        for (const Folder& f : session_.project().folders) {
            QJsonObject o;
            o[K("id")] = f.id;
            o[K("name")] = f.name;
            o[K("parent_id")] = f.parentId;
            list.append(o);
        }
        out[K("folders")] = list;
        return true;
    });

    reg("get_sources", [this](const QJsonObject&, QJsonObject& out, QString&) {
        QJsonArray list;
        for (const Source& s : session_.project().sources) {
            QJsonObject o;
            o[K("id")] = s.id;
            o[K("name")] = s.name;
            o[K("path")] = s.path;
            o[K("parent_id")] = s.parentId;
            list.append(o);
        }
        out[K("sources")] = list;
        return true;
    });

    reg("get_timelines", [this](const QJsonObject&, QJsonObject& out, QString&) {
        QJsonArray list;
        for (const Timeline& t : session_.project().timelines) {
            QJsonObject o;
            o[K("id")] = t.id;
            o[K("name")] = t.name;
            o[K("parent_id")] = t.parentId;
            o[K("is_default")] = t.isDefault;
            o[K("clip_count")] = static_cast<int>(t.clips.size());
            list.append(o);
        }
        out[K("timelines")] = list;
        return true;
    });

    reg("add_folder", [this](const QJsonObject& req, QJsonObject& out, QString& err) {
        QString name;
        if (!requireString(req, "name", name, err)) return false;
        const QString id = session_.project().addFolder(name, optString(req, "parent_id"));
        if (id.isEmpty()) {
            err = QStringLiteral("Parent folder not found");
            return false;
        }
        session_.notifyProjectChanged();
        out[K("id")] = id;
        return true;
    });

    reg("add_timeline", [this](const QJsonObject& req, QJsonObject& out, QString& err) {
        QString name;
        if (!requireString(req, "name", name, err)) return false;
        const QString id = session_.project().addTimeline(name, optString(req, "parent_id"));
        if (id.isEmpty()) {
            err = QStringLiteral("Parent folder not found");
            return false;
        }
        session_.notifyProjectChanged();
        out[K("id")] = id;
        return true;
    });

    reg("remove", [this](const QJsonObject& req, QJsonObject&, QString& err) {
        QString object;
        QString id;
        if (!requireString(req, "object", object, err) || !requireString(req, "id", id, err)) return false;
        const bool force = optBool(req, "force", false);

        // The id must belong to the kind of item that was named.
        const Project& project = session_.project();
        bool kindMatches = false;
        if (object == QLatin1String("folder")) {
            kindMatches = project.findFolder(id) != nullptr;
        } else if (object == QLatin1String("source")) {
            kindMatches = project.findSource(id) != nullptr;
        } else if (object == QLatin1String("timeline")) {
            kindMatches = project.findTimeline(id) != nullptr;
        } else {
            err = QStringLiteral("\"object\" must be \"folder\", \"source\" or \"timeline\"");
            return false;
        }
        // The session empties any viewer that shows what is removed and keeps playback consistent.
        const RemoveStatus status = kindMatches ? session_.removeItem(id, force) : RemoveStatus::NotFound;
        if (status != RemoveStatus::Ok) {
            err = removeError(status);
            return false;
        }
        return true;
    });

    // --- playback ----------------------------------------------------------------------
    reg("get_frame", [this](const QJsonObject&, QJsonObject& out, QString&) {
        out[K("frame")] = session_.playback().frame();
        out[K("frame_count")] = session_.playback().frameCount();
        return true;
    });

    reg("set_frame", [this](const QJsonObject& req, QJsonObject&, QString& err) {
        int frame = 0;
        if (!requireInt(req, "frame", frame, err)) return false;
        const int count = session_.playback().frameCount();
        if (count <= 0) {
            err = QStringLiteral("No source is loaded");
            return false;
        }
        if (frame < 0 || frame >= count) {
            err = QStringLiteral("Frame %1 is outside 0..%2").arg(frame).arg(count - 1);
            return false;
        }
        session_.playback().seek(frame);
        return true;
    });

    reg("set_playing", [this](const QJsonObject& req, QJsonObject&, QString& err) {
        if (!req.value(K("playing")).isBool()) {
            err = QStringLiteral("\"playing\" must be true or false");
            return false;
        }
        if (req.value(K("playing")).toBool()) {
            session_.playback().play(optBool(req, "play_forwards", true));
        } else {
            session_.playback().pause();
        }
        return true;
    });

    reg("get_range", [this](const QJsonObject&, QJsonObject& out, QString&) {
        out[K("range_start")] = session_.playback().rangeStart();
        out[K("range_end")] = session_.playback().rangeEnd();
        return true;
    });

    reg("set_range", [this](const QJsonObject& req, QJsonObject&, QString& err) {
        int start = 0;
        int end = 0;
        if (!requireInt(req, "start_frame", start, err) || !requireInt(req, "end_frame", end, err)) return false;
        const int count = session_.playback().frameCount();
        if (start < 0 || end < start || end >= count) {
            err = QStringLiteral("Invalid range %1..%2 (frame count %3)").arg(start).arg(end).arg(count);
            return false;
        }
        session_.playback().setRange(start, end);
        return true;
    });

    reg("get_config", [](const QJsonObject&, QJsonObject& out, QString&) {
        out[K("app_name")] = QCoreApplication::applicationName();
        out[K("version")] = QCoreApplication::applicationVersion();
        out[K("api_version")] = kApiVersion;
        return true;
    });

    // --- timelines ---------------------------------------------------------------------
    reg("get_default_timeline", [this](const QJsonObject&, QJsonObject& out, QString&) {
        const Timeline* timeline = session_.project().defaultTimeline();
        out[K("id")] = timeline ? timeline->id : QString();
        return true;
    });

    reg("set_default_timeline", [this](const QJsonObject& req, QJsonObject&, QString& err) {
        QString id;
        if (!requireString(req, "id", id, err)) return false;
        if (!session_.project().findTimeline(id)) {
            err = QStringLiteral("Timeline not found");
            return false;
        }
        session_.setDefaultTimeline(id);
        return true;
    });

    reg("get_timeline_elements", [this](const QJsonObject& req, QJsonObject& out, QString& err) {
        QString id;
        if (!requireString(req, "timeline_id", id, err)) return false;
        const Timeline* timeline = session_.project().findTimeline(id);
        if (!timeline) {
            err = QStringLiteral("Timeline not found");
            return false;
        }
        QJsonArray list;
        for (const Clip& clip : timeline->clips) {
            QJsonObject o;
            o[K("source_id")] = clip.sourceId;
            const Source* source = session_.project().findSource(clip.sourceId);
            o[K("name")] = source ? source->name : QString();
            list.append(o);
        }
        out[K("elements")] = list;
        return true;
    });

    reg("insert_element_in_timeline", [this](const QJsonObject& req, QJsonObject&, QString& err) {
        QString timelineId;
        QString sourceId;
        if (!requireString(req, "timeline_id", timelineId, err) || !requireString(req, "source_id", sourceId, err)) {
            return false;
        }
        const Timeline* timeline = session_.project().findTimeline(timelineId);
        if (!timeline || !session_.project().findSource(sourceId)) {
            err = QStringLiteral("Timeline or source not found");
            return false;
        }
        const int index = req.value(K("index")).isDouble() ? req.value(K("index")).toInt() : -1;
        if (index > timeline->clips.size()) {
            err = QStringLiteral("Index %1 is past the end of the timeline").arg(index);
            return false;
        }
        session_.addClipToTimeline(timelineId, sourceId, index);
        return true;
    });

    reg("remove_element_from_timeline", [this](const QJsonObject& req, QJsonObject&, QString& err) {
        QString timelineId;
        int index = 0;
        if (!requireString(req, "timeline_id", timelineId, err) || !requireInt(req, "index", index, err)) return false;
        const Timeline* timeline = session_.project().findTimeline(timelineId);
        if (!timeline || index < 0 || index >= timeline->clips.size()) {
            err = QStringLiteral("No such element");
            return false;
        }
        session_.removeClipFromTimeline(timelineId, index);
        return true;
    });

    // --- viewers -----------------------------------------------------------------------
    reg("get_active_in_viewer", [this](const QJsonObject& req, QJsonObject& out, QString&) {
        const int viewer = req.value(K("viewer")).isDouble() ? req.value(K("viewer")).toInt() : -1;
        out[K("kind")] = session_.viewerTargetKind(viewer);
        out[K("id")] = session_.viewerTargetId(viewer);
        return true;
    });

    reg("set_active_in_viewer", [this](const QJsonObject& req, QJsonObject&, QString& err) {
        QString id;
        if (!requireString(req, "id", id, err)) return false;
        if (req.value(K("viewer")).isDouble()) {
            const int viewer = req.value(K("viewer")).toInt();
            if (viewer < 0 || viewer > 1) {
                err = QStringLiteral("\"viewer\" must be 0 or 1");
                return false;
            }
            if (viewer == 1 && session_.viewerCount() < 2) session_.setViewerLayout(QStringLiteral("horizontal"));
            session_.setActiveViewer(viewer);
        }
        if (session_.project().findTimeline(id)) return session_.activateTimeline(id, &err);
        if (session_.project().findSource(id)) return session_.activateSource(id, &err);
        err = QStringLiteral("No source or timeline with this id");
        return false;
    });

    reg("set_viewer_layout", [this](const QJsonObject& req, QJsonObject&, QString& err) {
        QString layout;
        if (!requireString(req, "layout", layout, err)) return false;
        static const QStringList layouts = {"single", "horizontal", "vertical", "split_horizontal", "split_vertical"};
        if (!layouts.contains(layout)) {
            err = QStringLiteral("\"layout\" must be one of: %1").arg(layouts.join(QStringLiteral(", ")));
            return false;
        }
        session_.setViewerLayout(layout);
        return true;
    });

    // --- the source under the playhead: bookmarks, notes, TC offset ----------------------
    // Frames here are the source's own frames, 0-based.
    const auto activeSource = [this](QString& err) -> Source* {
        Source* source = session_.project().findSource(session_.activeSourceId());
        if (!source) err = QStringLiteral("No source is shown");
        return source;
    };

    reg("get_bookmarks", [this, activeSource](const QJsonObject&, QJsonObject& out, QString& err) {
        const Source* source = activeSource(err);
        if (!source) return false;
        QJsonArray frames;
        for (const int frame : source->frameBookmarks) frames.append(frame);
        QJsonArray ranges;
        for (const RangeBookmark& range : source->rangeBookmarks) ranges.append(QJsonArray{range.start, range.end});
        out[K("source_id")] = source->id;
        out[K("frames")] = frames;
        out[K("ranges")] = ranges;
        return true;
    });

    reg("create_bookmarks", [this, activeSource](const QJsonObject& req, QJsonObject&, QString& err) {
        Source* source = activeSource(err);
        if (!source) return false;
        for (const QJsonValue& value : req.value(K("frames")).toArray()) {
            if (value.isDouble()) source->frameBookmarks.push_back(value.toInt());
        }
        for (const QJsonValue& value : req.value(K("ranges")).toArray()) {
            const QJsonArray pair = value.toArray();
            if (pair.size() == 2) source->rangeBookmarks.push_back(RangeBookmark{pair[0].toInt(), pair[1].toInt()});
        }
        source->normalizeBookmarks();
        session_.notifyBookmarksChanged();
        return true;
    });

    reg("clear_bookmarks", [this, activeSource](const QJsonObject&, QJsonObject&, QString& err) {
        if (!activeSource(err)) return false;
        session_.clearBookmarks();
        return true;
    });

    reg("add_notes", [this, activeSource](const QJsonObject& req, QJsonObject&, QString& err) {
        Source* source = activeSource(err);
        if (!source) return false;
        const QJsonArray notes = req.value(K("notes")).toArray();
        if (notes.isEmpty()) {
            err = QStringLiteral("\"notes\" must be a list of {\"frame\": n, \"text\": \"...\"}");
            return false;
        }
        for (const QJsonValue& value : notes) {
            const QJsonObject note = value.toObject();
            if (!note.value(K("frame")).isDouble()) continue;
            source->annotationForEdit(note.value(K("frame")).toInt()).note = note.value(K("text")).toString();
        }
        source->pruneEmptyAnnotations();
        session_.notifyAnnotationsChanged();
        return true;
    });

    reg("get_tc_frame_offset_override", [this, activeSource](const QJsonObject&, QJsonObject& out, QString& err) {
        const Source* source = activeSource(err);
        if (!source) return false;
        out[K("enabled")] = source->tcOffsetOverrideEnabled;
        out[K("offset")] = source->tcOffsetOverride;
        return true;
    });

    reg("set_tc_frame_offset_override", [this, activeSource](const QJsonObject& req, QJsonObject&, QString& err) {
        if (!activeSource(err)) return false;
        session_.setTcOffsetOverride(optBool(req, "enabled", true), req.value(K("offset")).toInt());
        return true;
    });
}

}  // namespace fr
