#include "api/ApiDispatcher.h"
#include "core/KproFormat.h"
#include "core/PlaybackController.h"
#include "core/ProjectModel.h"
#include "core/Session.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

using namespace fr;

namespace {

QJsonObject request(const char* cmd, QJsonObject params = {}) {
    params.insert(QStringLiteral("cmd"), QString::fromLatin1(cmd));
    return params;
}

}  // namespace

class CoreTests : public QObject {
    Q_OBJECT

private slots:
    // --- playback rules --------------------------------------------------------------
    void advance_off_stops_on_the_last_frame() {
        const StepResult r = advanceFrames(3, 1, 5, 0, 4, LoopMode::Off);
        QCOMPARE(r.frame, 4);
        QVERIFY(r.stopped);

        const StepResult back = advanceFrames(1, -1, 1, 0, 4, LoopMode::Off);
        QCOMPARE(back.frame, 0);
        QVERIFY(!back.stopped);
    }

    void advance_loop_wraps_both_ways() {
        QCOMPARE(advanceFrames(4, 1, 1, 2, 4, LoopMode::Loop).frame, 2);
        QCOMPARE(advanceFrames(2, -1, 1, 2, 4, LoopMode::Loop).frame, 4);
    }

    void advance_pingpong_reverses_without_repeating_the_end() {
        // 2 -> 3 -> 2 -> 1 -> 0 -> 1
        const StepResult r = advanceFrames(2, 1, 5, 0, 3, LoopMode::PingPong);
        QCOMPARE(r.frame, 1);
        QCOMPARE(r.direction, 1);
        QVERIFY(!r.stopped);
        QVERIFY(advanceFrames(0, 1, 1, 0, 0, LoopMode::PingPong).stopped);
    }

    // --- project model ---------------------------------------------------------------
    void stepped_frame_holds_the_last_key() {
        const QVector<int> keys{2, 10, 30};
        QCOMPARE(steppedFrame(keys, 5, 0, 40), 2);
        QCOMPARE(steppedFrame(keys, 10, 0, 40), 10);
        QCOMPARE(steppedFrame(keys, 39, 0, 40), 30);
        QCOMPARE(steppedFrame(keys, 1, 0, 40), 30);   // before the first key: the pose the loop left
        QCOMPARE(steppedFrame(keys, 12, 5, 20), 10);  // keys outside the range are ignored
        QCOMPARE(steppedFrame(keys, 6, 5, 20), 10);
        QCOMPARE(steppedFrame(keys, 12, 11, 20), -1);  // no key in the range: plays normally
        QCOMPARE(steppedFrame({}, 3, 0, 40), -1);
    }

    void project_starts_with_the_base_folders() {
        const Project p = Project::createEmpty();
        QVERIFY(p.findFolder(kSourcesFolderId));
        QVERIFY(p.findFolder(kTimelinesFolderId));
        QVERIFY(Project::isBaseFolder(kSourcesFolderId));
    }

    void project_remove_rules() {
        Project p = Project::createEmpty();
        const QString source = p.addSource(QStringLiteral("/media/a.mov"));
        const QString timeline = p.addTimeline(QStringLiteral("Main"));
        p.findTimeline(timeline)->clips.push_back(Clip{source});

        QCOMPARE(p.removeSource(source, false), RemoveStatus::SourceInTimelines);
        QCOMPARE(p.removeSource(source, true), RemoveStatus::Ok);
        QVERIFY(p.findTimeline(timeline)->clips.isEmpty());

        QCOMPARE(p.removeFolder(kSourcesFolderId), RemoveStatus::BaseFolder);
        const QString folder = p.addFolder(QStringLiteral("Shots"));
        p.addSource(QStringLiteral("/media/b.mov"), QString(), folder);
        QCOMPARE(p.removeFolder(folder), RemoveStatus::NotEmpty);
        QCOMPARE(p.removeFolder(QStringLiteral("nope")), RemoveStatus::NotFound);
    }

    void bookmarks_are_normalised_and_navigated() {
        Source source;
        source.frameBookmarks = {9, 3, 3, 12, -2};
        source.rangeBookmarks = {RangeBookmark{20, 25}, RangeBookmark{10, 5}, RangeBookmark{20, 25},
                                 RangeBookmark{-1, 4}};
        source.normalizeBookmarks();
        QCOMPARE(source.frameBookmarks, (QVector<int>{3, 9, 12}));  // sorted, unique, no negatives
        QCOMPARE(source.rangeBookmarks, (QVector<RangeBookmark>{RangeBookmark{20, 25}}));
        QCOMPARE(source.bookmarkTargets(), (QVector<int>{3, 9, 12, 20}));  // frames and range starts

        QCOMPARE(source.adjacentBookmark(0, 1).value_or(-1), 3);
        QCOMPARE(source.adjacentBookmark(3, 1).value_or(-1), 9);
        QCOMPARE(source.adjacentBookmark(12, 1).value_or(-1), 20);
        QCOMPARE(source.adjacentBookmark(20, 1).value_or(-1), 3);   // wraps forward
        QCOMPARE(source.adjacentBookmark(3, -1).value_or(-1), 20);  // wraps backward
        QCOMPARE(source.adjacentBookmark(9, -1).value_or(-1), 3);
        QCOMPARE(source.adjacentBookmark(5, -1).value_or(-1), 3);

        const Source none;
        QVERIFY(!none.adjacentBookmark(5, 1).has_value());
        QVERIFY(!none.adjacentBookmark(5, -1).has_value());
    }

    void project_first_timeline_becomes_default() {
        Project p = Project::createEmpty();
        const QString first = p.addTimeline(QStringLiteral("One"));
        const QString second = p.addTimeline(QStringLiteral("Two"));
        QCOMPARE(p.defaultTimeline()->id, first);
        QVERIFY(p.setDefaultTimeline(second));
        QCOMPARE(p.defaultTimeline()->id, second);
        QCOMPARE(p.removeTimeline(second), RemoveStatus::Ok);
        QCOMPARE(p.defaultTimeline()->id, first);
    }

    // --- project file ----------------------------------------------------------------
    void kpro_round_trip() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        Project p = Project::createEmpty();
        p.sourceRootDir = dir.path();
        p.imageSeqFrameRate = 25.0;
        const QString folder = p.addFolder(QStringLiteral("Shots"));
        const QString sourceId = p.addSource(dir.filePath(QStringLiteral("clip.mov")), QString(), folder);

        Source* source = p.findSource(sourceId);
        source->cutInFrame = 3;
        source->frameBookmarks = {5, 9};
        source->rangeBookmarks.push_back(RangeBookmark{10, 20});
        FrameAnnotation& annotation = source->annotationForEdit(7);
        annotation.note = QStringLiteral("Fix the arc");
        annotation.noteLabel = QStringLiteral("A");
        Stroke stroke;
        stroke.type = Stroke::Type::Arrow;
        stroke.color = QColor(255, 0, 0, 128);
        stroke.width = 6.5;
        stroke.points = {{0.1, 0.2}, {0.5, 0.75}};
        annotation.strokes.push_back(stroke);

        const QString timelineId = p.addTimeline(QStringLiteral("Main"));
        p.findTimeline(timelineId)->clips.push_back(Clip{sourceId});

        const QString path = dir.filePath(QStringLiteral("proj/review.kpro"));
        QString error;
        QVERIFY2(saveProject(p, path, &error), qPrintable(error));

        // The file is plain JSON with the expected top-level layout.
        QFile raw(path);
        QVERIFY(raw.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(raw.readAll()).object();
        QVERIFY(root.contains(QStringLiteral("_meta")));
        QVERIFY(root.contains(QStringLiteral("_project")));
        QJsonObject savedSource;
        for (const QJsonValue& v : root.value(QStringLiteral("sources")).toArray()) {
            if (v.toObject().value(QStringLiteral("_id")).toString() == sourceId) savedSource = v.toObject();
        }
        QCOMPARE(savedSource.value(QStringLiteral("relative_path")).toString(), QStringLiteral("../clip.mov"));
        const QJsonObject savedStroke = savedSource.value(QStringLiteral("frame_data"))
                                            .toArray().first().toObject()
                                            .value(QStringLiteral("strokes")).toArray().first().toObject();
        QCOMPARE(savedStroke.value(QStringLiteral("points")).toString(), QStringLiteral("0.1,0.2;0.5,0.75"));

        const LoadResult loaded = loadProject(path);
        QVERIFY2(loaded.ok, qPrintable(loaded.error));
        QVERIFY(loaded.warnings.isEmpty());
        QCOMPARE(loaded.project.imageSeqFrameRate, 25.0);

        const Source* back = loaded.project.findSource(sourceId);
        QVERIFY(back);
        QCOMPARE(back->parentId, folder);
        QCOMPARE(back->cutInFrame, 3);
        QCOMPARE(back->frameBookmarks, (QVector<int>{5, 9}));
        QCOMPARE(back->rangeBookmarks.size(), 1);
        QCOMPARE(back->rangeBookmarks.first().end, 20);
        const FrameAnnotation* note = back->annotationAt(7);
        QVERIFY(note);
        QCOMPARE(note->note, QStringLiteral("Fix the arc"));
        QCOMPARE(note->noteLabel, QStringLiteral("A"));
        QCOMPARE(note->strokes.size(), 1);
        QCOMPARE(note->strokes.first().type, Stroke::Type::Arrow);
        QCOMPARE(note->strokes.first().color.alpha(), 128);
        QCOMPARE(note->strokes.first().width, 6.5);
        QCOMPARE(note->strokes.first().points, (QVector<QPointF>{{0.1, 0.2}, {0.5, 0.75}}));

        QVERIFY(loaded.project.findTimeline(timelineId));
        QCOMPARE(loaded.project.findTimeline(timelineId)->clips.first().sourceId, sourceId);
        QVERIFY(loaded.project.defaultTimeline());
    }

    void kpro_normalises_bookmarks_on_load() {
        Project p = Project::createEmpty();
        const QString id = p.addSource(QStringLiteral("/media/a.mov"));
        Source* source = p.findSource(id);
        source->frameBookmarks = {8, 2, 8};  // a file may hold them in any order
        source->rangeBookmarks = {RangeBookmark{6, 7}, RangeBookmark{1, 3}};

        const LoadResult loaded = projectFromJson(projectToJson(p));
        QVERIFY(loaded.ok);
        const Source* back = loaded.project.findSource(id);
        QVERIFY(back);
        QCOMPARE(back->frameBookmarks, (QVector<int>{2, 8}));
        QCOMPARE(back->rangeBookmarks.size(), 2);
        QCOMPARE(back->rangeBookmarks.first().start, 1);
    }

    void kpro_rejects_a_file_without_meta() {
        const LoadResult r = projectFromJson(QJsonObject());
        QVERIFY(!r.ok);
        QVERIFY(!r.error.isEmpty());
    }

    void kpro_skips_duplicate_ids_with_a_warning() {
        const auto folder = [](const char* name) {
            QJsonObject o;
            o.insert(QStringLiteral("_id"), QStringLiteral("x"));
            o.insert(QStringLiteral("name"), QString::fromLatin1(name));
            o.insert(QStringLiteral("parent_id"), kSourcesFolderId);
            return o;
        };
        QJsonObject meta;
        meta.insert(QStringLiteral("format"), QStringLiteral("kpro"));
        QJsonObject root;
        root.insert(QStringLiteral("_meta"), meta);
        root.insert(QStringLiteral("folders"), QJsonArray{folder("One"), folder("Two")});

        const LoadResult r = projectFromJson(root);
        QVERIFY(r.ok);
        QCOMPARE(r.project.folders.size(), 3);  // two base folders + "One"
        QCOMPARE(r.project.findFolder(QStringLiteral("x"))->name, QStringLiteral("One"));
        QCOMPARE(r.warnings.size(), 1);
    }

    void kpro_breaks_folder_cycles() {
        const auto folder = [](const char* id, const char* parent) {
            QJsonObject o;
            o.insert(QStringLiteral("_id"), QString::fromLatin1(id));
            o.insert(QStringLiteral("name"), QString::fromLatin1(id));
            o.insert(QStringLiteral("parent_id"), QString::fromLatin1(parent));
            return o;
        };
        QJsonObject meta;
        meta.insert(QStringLiteral("format"), QStringLiteral("kpro"));
        QJsonObject root;
        root.insert(QStringLiteral("_meta"), meta);
        root.insert(QStringLiteral("folders"), QJsonArray{folder("a", "b"), folder("b", "a")});

        const LoadResult r = projectFromJson(root);
        QVERIFY(r.ok);
        QVERIFY(!r.warnings.isEmpty());
        // At least one of them now hangs off a base folder, so the tree has a root again.
        const bool rooted = r.project.findFolder(QStringLiteral("a"))->parentId == kSourcesFolderId ||
                            r.project.findFolder(QStringLiteral("b"))->parentId == kSourcesFolderId;
        QVERIFY(rooted);
    }

    void kpro_resolves_source_paths() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("media")));
        const QString media = dir.filePath(QStringLiteral("media/a.txt"));
        QFile file(media);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        const QString projectFile = dir.filePath(QStringLiteral("review.kpro"));

        Project p = Project::createEmpty();
        Source viaRelative;
        viaRelative.path = QStringLiteral("/nonexistent/a.txt");
        viaRelative.relativePath = QStringLiteral("media/a.txt");
        QCOMPARE(resolveSourcePath(viaRelative, p, projectFile), media);

        Source viaToken;
        viaToken.path = kProjectDirToken + QStringLiteral("/media/a.txt");
        QCOMPARE(resolveSourcePath(viaToken, p, projectFile), media);

        Source missing;
        missing.path = QStringLiteral("/nonexistent/b.txt");
        QCOMPARE(resolveSourcePath(missing, p, projectFile), missing.path);
    }

    // --- Client API ------------------------------------------------------------------
    void api_refuses_commands_before_initialize() {
        Session session;
        ApiDispatcher api(session);
        QVERIFY(!api.handle(request("get_folders")).value(QStringLiteral("success")).toBool());
        QVERIFY(api.handle(request("echo")).value(QStringLiteral("success")).toBool());
    }

    void api_basic_flow() {
        Session session;
        ApiDispatcher api(session);

        QJsonObject params;
        params.insert(QStringLiteral("api_version"), 1);
        const QJsonObject init = api.handle(request("initialize", params));
        QVERIFY(init.value(QStringLiteral("success")).toBool());
        QVERIFY(init.value(QStringLiteral("result")).toObject().contains(QStringLiteral("temp_dir")));
        QVERIFY(!api.handle(request("initialize", params)).value(QStringLiteral("success")).toBool());

        QJsonObject folderParams;
        folderParams.insert(QStringLiteral("name"), QStringLiteral("Shots"));
        const QJsonObject added = api.handle(request("add_folder", folderParams));
        QVERIFY(added.value(QStringLiteral("success")).toBool());

        const QJsonObject folders = api.handle(request("get_folders"));
        QCOMPARE(folders.value(QStringLiteral("result")).toObject().value(QStringLiteral("folders")).toArray().size(), 3);

        QJsonObject removeBase;
        removeBase.insert(QStringLiteral("object"), QStringLiteral("folder"));
        removeBase.insert(QStringLiteral("id"), kSourcesFolderId);
        QVERIFY(!api.handle(request("remove", removeBase)).value(QStringLiteral("success")).toBool());

        const QJsonObject unknown = api.handle(request("no_such_command"));
        QVERIFY(!unknown.value(QStringLiteral("success")).toBool());
        const QJsonObject planned = api.handle(request("queue_export"));
        QVERIFY(planned.value(QStringLiteral("msg")).toString().contains(QStringLiteral("not implemented")));

        // Timelines through the API.
        QJsonObject timelineParams;
        timelineParams.insert(QStringLiteral("name"), QStringLiteral("Edit"));
        const QString timelineId = api.handle(request("add_timeline", timelineParams))
                                       .value(QStringLiteral("result")).toObject().value(QStringLiteral("id")).toString();
        QVERIFY(!timelineId.isEmpty());
        QJsonObject idParams;
        idParams.insert(QStringLiteral("id"), timelineId);
        QVERIFY(api.handle(request("set_default_timeline", idParams)).value(QStringLiteral("success")).toBool());
        QCOMPARE(api.handle(request("get_default_timeline")).value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("id")).toString(), timelineId);
        QJsonObject elements;
        elements.insert(QStringLiteral("timeline_id"), timelineId);
        QCOMPARE(api.handle(request("get_timeline_elements", elements)).value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("elements")).toArray().size(), 0);

        // Without a source on screen the source commands say so instead of failing silently.
        QVERIFY(!api.handle(request("get_bookmarks")).value(QStringLiteral("success")).toBool());
        QJsonObject layout;
        layout.insert(QStringLiteral("layout"), QStringLiteral("sideways"));
        QVERIFY(!api.handle(request("set_viewer_layout", layout)).value(QStringLiteral("success")).toBool());
        layout.insert(QStringLiteral("layout"), QStringLiteral("horizontal"));
        QVERIFY(api.handle(request("set_viewer_layout", layout)).value(QStringLiteral("success")).toBool());
        QCOMPARE(session.viewerCount(), 2);
    }

    void api_rejects_an_unsupported_version() {
        Session session;
        ApiDispatcher api(session);
        QJsonObject params;
        params.insert(QStringLiteral("api_version"), 99);
        QVERIFY(!api.handle(request("initialize", params)).value(QStringLiteral("success")).toBool());
        QVERIFY(!api.initialized());
    }
};

QTEST_GUILESS_MAIN(CoreTests)
#include "test_core.moc"
