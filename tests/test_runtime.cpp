// Tests that need a running event loop, image files or a real video: the playback timer,
// the still-image source and the FFmpeg decoder.
#include "core/PlaybackController.h"
#include "core/Session.h"
#include "media/AsyncFrameSource.h"
#include "media/FrameSource.h"

#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include <atomic>
#include <memory>

using namespace fr;

namespace {

// A source that takes `delayMs` per frame. The red channel of every pixel is the frame number, so
// a test can tell which frame it got. Frames from `failFrom` on cannot be decoded.
class SlowSource final : public FrameSource {
public:
    SlowSource(int frames, int delayMs, int failFrom = -1)
        : frames_(frames), delayMs_(delayMs), failFrom_(failFrom) {}
    bool open(const QString&, QString*) override { return true; }
    int frameCount() const override { return frames_; }
    double fps() const override { return 24.0; }
    QSize size() const override { return QSize(4, 4); }
    QImage frame(int index) override {
        QThread::msleep(static_cast<unsigned long>(delayMs_));
        ++attempts_;
        if (failFrom_ >= 0 && index >= failFrom_) return {};
        QImage image(4, 4, QImage::Format_RGB32);
        image.fill(qRgb(index, 0, 0));
        return image;
    }
    int attempts() const { return attempts_; }
    bool concurrentFrames() const override { return concurrent_; }
    void setConcurrent(bool concurrent) { concurrent_ = concurrent; }

private:
    bool concurrent_ = false;
    int frames_;
    int delayMs_;
    int failFrom_;
    std::atomic<int> attempts_{0};
};

int redOf(const QImage& image) {
    return qRed(image.pixel(0, 0));
}

// 30 frames at 10 fps, lossless; frame n has luma 8*n so each frame can be told apart.
// Returns the path, or an empty string when ffmpeg is missing or fails.
QString makeRampVideo(const QString& dir) {
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) return {};
    const QString path = QDir(dir).filePath(QStringLiteral("ramp.mkv"));
    QProcess make;
    make.start(ffmpeg, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-f"),
                        QStringLiteral("lavfi"), QStringLiteral("-i"),
                        QStringLiteral("color=c=black:s=64x48:r=10:d=3"), QStringLiteral("-vf"),
                        QStringLiteral("geq=lum='8*N':cb=128:cr=128"), QStringLiteral("-c:v"),
                        QStringLiteral("ffv1"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
                        QStringLiteral("-y"), path});
    if (!make.waitForFinished(30000) || make.exitCode() != 0) return {};
    return path;
}

// Opens the 30-frame ramp video as the session's active source. Returns the source id, or an
// empty string when the video could not be made or opened.
QString openRamp(Session& session, const QString& dir) {
    const QString path = makeRampVideo(dir);
    if (path.isEmpty()) return {};
    const QString id = session.importFile(path);
    return session.activateSource(id) ? id : QString();
}

// Expected grey level of a frame whose luma is 8*n, after the limited-range YUV -> RGB step.
int expectedGray(int n) {
    const double y = 8.0 * n;
    return qBound(0, qRound((y - 16.0) * 255.0 / 219.0), 255);
}

}  // namespace

class RuntimeTests : public QObject {
    Q_OBJECT

private slots:
    void still_image_source_opens_a_png() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("blue.png"));
        QImage image(8, 6, QImage::Format_RGB32);
        image.fill(Qt::blue);
        QVERIFY(image.save(path));

        QString error;
        std::unique_ptr<FrameSource> source = openFrameSource(path, &error);
        QVERIFY2(source, qPrintable(error));
        QCOMPARE(source->frameCount(), 1);
        QCOMPARE(source->size(), QSize(8, 6));
        QCOMPARE(source->fps(), 0.0);
        QCOMPARE(source->frame(0).pixel(0, 0), QColor(Qt::blue).rgb());
    }

    void missing_file_reports_an_error() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QString error;
        QVERIFY(!openFrameSource(dir.filePath(QStringLiteral("nope.mp4")), &error));
        QVERIFY(!error.isEmpty());
    }

    void playback_follows_the_clock_and_stops_at_the_end() {
        PlaybackController playback;
        playback.configure(10, 200.0);  // 10 frames at 200 fps: about 50 ms
        playback.setLoopMode(LoopMode::Off);
        QSignalSpy playing(&playback, &PlaybackController::playingChanged);

        playback.play();
        QTRY_VERIFY_WITH_TIMEOUT(!playback.isPlaying(), 2000);
        QCOMPARE(playback.frame(), 9);
        QCOMPARE(playing.count(), 2);  // started, then stopped by reaching the end
    }

    void playback_waits_for_frames_that_are_not_ready() {
        PlaybackController playback;
        playback.configure(10, 200.0);
        playback.setLoopMode(LoopMode::Off);
        bool open = false;
        playback.setFrameGate([&open](int frame) { return frame < 3 || open; });
        playback.play();
        QTest::qWait(150);
        QCOMPARE(playback.frame(), 2);  // frame 3 is not decoded: wait on 2, do not skip
        QVERIFY(playback.isPlaying());
        open = true;
        QTRY_VERIFY_WITH_TIMEOUT(!playback.isPlaying(), 2000);
        QCOMPARE(playback.frame(), 9);

        // With the sound as the clock the picture never waits.
        playback.setFrameGate([](int) { return false; });
        playback.setRealtime(true);
        playback.play();
        QTRY_VERIFY_WITH_TIMEOUT(!playback.isPlaying(), 2000);
        QCOMPARE(playback.frame(), 9);
    }

    void playback_loops_inside_the_range() {
        PlaybackController playback;
        playback.configure(20, 200.0);
        playback.setRange(5, 9);
        playback.setLoopMode(LoopMode::Loop);
        QSignalSpy frames(&playback, &PlaybackController::frameChanged);

        playback.play();  // starts outside the range, so it must jump to its start
        QTest::qWait(200);
        playback.pause();

        QVERIFY(playback.frame() >= 5 && playback.frame() <= 9);
        QVERIFY(frames.count() > 5);  // more steps than the range is long: it wrapped
        for (const QList<QVariant>& call : frames) {
            const int frame = call.first().toInt();
            QVERIFY2(frame >= 5 && frame <= 9, qPrintable(QString::number(frame)));
        }
    }

    // --- drawing: undo logic on a Session with a still image as the active source -------------
    void strokes_are_added_undone_redone_and_cleared() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("still.png"));
        QImage image(8, 6, QImage::Format_RGB32);
        image.fill(Qt::gray);
        QVERIFY(image.save(path));

        Session session;
        const QString id = session.importFile(path);
        QVERIFY(session.activateSource(id));
        QSignalSpy changed(&session, &Session::annotationsChanged);

        Stroke stroke;
        stroke.points = {{0.1, 0.1}, {0.9, 0.9}};
        const auto strokeCount = [&session, &id] {
            const FrameAnnotation* a = session.project().findSource(id)->annotationAt(0);
            return a ? static_cast<int>(a->strokes.size()) : 0;
        };

        session.addStroke(stroke);
        session.addStroke(stroke);
        QCOMPARE(strokeCount(), 2);
        session.undoStack().undo();
        QCOMPARE(strokeCount(), 1);
        session.undoStack().undo();
        QCOMPARE(strokeCount(), 0);
        QVERIFY(!session.project().findSource(id)->annotationAt(0));  // an empty annotation is dropped
        session.undoStack().redo();
        QCOMPARE(strokeCount(), 1);
        session.clearStrokes();
        QCOMPARE(strokeCount(), 0);
        session.undoStack().undo();
        QCOMPARE(strokeCount(), 1);
        QVERIFY(changed.count() >= 6);
    }

    void undoing_the_last_stroke_keeps_the_note() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("still.png"));
        QImage image(8, 6, QImage::Format_RGB32);
        image.fill(Qt::gray);
        QVERIFY(image.save(path));

        Session session;
        const QString id = session.importFile(path);
        QVERIFY(session.activateSource(id));
        session.project().findSource(id)->annotationForEdit(0).note = QStringLiteral("keep me");

        Stroke stroke;
        stroke.points = {{0.2, 0.2}, {0.4, 0.4}};
        session.addStroke(stroke);
        session.undoStack().undo();

        const FrameAnnotation* annotation = session.project().findSource(id)->annotationAt(0);
        QVERIFY(annotation);
        QCOMPARE(annotation->note, QStringLiteral("keep me"));
        QVERIFY(annotation->strokes.isEmpty());
    }

    void strokes_without_an_active_source_are_ignored() {
        Session session;
        QSignalSpy changed(&session, &Session::annotationsChanged);
        Stroke stroke;
        stroke.points = {{0.1, 0.1}, {0.2, 0.2}};
        session.addStroke(stroke);
        session.clearStrokes();
        QCOMPARE(changed.count(), 0);
        QVERIFY(!session.undoStack().canUndo());
    }

    // --- decoding on a worker thread ---------------------------------------------------------
    void async_calls_return_at_once_while_the_decoder_is_slow() {
        AsyncFrameSource async(std::make_unique<SlowSource>(200, 40), 0);  // frame 0 is decoded here
        QSignalSpy ready(&async, &AsyncFrameSource::frameReady);

        QElapsedTimer timer;
        timer.start();
        async.request(100, 1);
        const QImage shown = async.nearestFrame(100);
        const qint64 elapsedMs = timer.elapsed();
        QVERIFY2(elapsedMs < 25, qPrintable(QStringLiteral("request+nearestFrame took %1 ms").arg(elapsedMs)));

        QVERIFY(!shown.isNull());          // stale, but never blank
        QVERIFY(redOf(shown) < 100);       // it is not the requested frame yet
        QVERIFY(async.exactFrame(100).isNull());

        QTRY_VERIFY_WITH_TIMEOUT(!async.exactFrame(100).isNull(), 3000);
        QCOMPARE(redOf(async.exactFrame(100)), 100);
        QVERIFY(ready.count() >= 1);
    }

    void async_prefetches_in_the_playing_direction() {
        AsyncFrameSource async(std::make_unique<SlowSource>(200, 2), 0, nullptr, 16);  // 8 ahead, 4 behind
        async.request(50, 1);
        QTRY_VERIFY_WITH_TIMEOUT(!async.exactFrame(58).isNull(), 3000);
        QCOMPARE(redOf(async.exactFrame(55)), 55);

        async.request(150, -1);  // playing backwards: the frames before 150 are the ones to have
        QTRY_VERIFY_WITH_TIMEOUT(!async.exactFrame(142).isNull(), 3000);
        QCOMPARE(redOf(async.exactFrame(145)), 145);
        QVERIFY(async.cachedCount() <= async.cacheLimit());
    }

    // A looping range that fits in the cache is decoded once and then stays: going round the loop
    // drops nothing in it (this was the cause of choppy image sequences).
    void async_holds_a_looping_range_that_fits() {
        AsyncFrameSource async(std::make_unique<SlowSource>(100, 1), 40, nullptr, 16);
        async.setPlaybackWindow(40, 49, true);
        async.request(45, 1);
        for (int f = 40; f <= 49; ++f) QTRY_VERIFY_WITH_TIMEOUT(async.isReady(f), 3000);
        for (const int f : {48, 49, 40, 41, 42, 43}) {
            async.request(f, 1);
            QTest::qWait(10);
        }
        for (int f = 40; f <= 49; ++f) QVERIFY2(!async.exactFrame(f).isNull(), qPrintable(QString::number(f)));
        QVERIFY(async.cachedCount() <= 16);

        // Near the end of a looping range the frames after the wrap are read ahead, not those past it.
        AsyncFrameSource wide(std::make_unique<SlowSource>(100, 1), 0, nullptr, 8);
        wide.setPlaybackWindow(10, 39, true);
        wide.request(38, 1);
        QTRY_VERIFY_WITH_TIMEOUT(wide.isReady(10) && wide.isReady(12), 3000);
        QVERIFY(wide.exactFrame(40).isNull());
    }

    void async_decodes_image_sequences_on_several_threads() {
        auto source = std::make_unique<SlowSource>(100, 30);
        source->setConcurrent(true);
        AsyncFrameSource async(std::move(source), 0, nullptr, 40);
        if (async.workerCount() < 2) QSKIP("one core: nothing to run in parallel");
        QElapsedTimer timer;
        timer.start();
        async.request(1, 1);
        for (int f = 1; f <= 12; ++f) QTRY_VERIFY_WITH_TIMEOUT(async.isReady(f), 3000);
        QVERIFY2(timer.elapsed() < 12 * 30, qPrintable(QString::number(timer.elapsed())));  // faster than one by one
    }

    void async_cache_stays_within_its_limit_and_drops_the_far_frames() {
        AsyncFrameSource async(std::make_unique<SlowSource>(200, 1), 0, nullptr, 8);
        QCOMPARE(async.cacheLimit(), 8);
        async.request(20, 1);
        QTRY_VERIFY_WITH_TIMEOUT(!async.exactFrame(24).isNull(), 3000);

        async.request(150, 1);
        QTRY_VERIFY_WITH_TIMEOUT(!async.exactFrame(154).isNull(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!async.exactFrame(149).isNull(), 3000);
        QVERIFY(async.cachedCount() <= 8);
        QVERIFY(async.exactFrame(0).isNull());  // far from the playhead: dropped
        QCOMPARE(redOf(async.nearestFrame(152)), 152);
    }

    void async_does_not_retry_frames_that_cannot_be_decoded() {
        auto source = std::make_unique<SlowSource>(20, 1, 10);  // frames 10.. fail
        SlowSource* raw = source.get();
        AsyncFrameSource async(std::move(source), 0);
        async.request(15, 1);
        QTest::qWait(300);
        const int attempts = raw->attempts();
        QTest::qWait(200);
        QCOMPARE(raw->attempts(), attempts);  // no retry loop
        QVERIFY(async.exactFrame(15).isNull());
        QVERIFY(!async.nearestFrame(15).isNull());  // it still shows the closest frame it has
    }

    void async_destruction_with_work_pending_is_prompt() {
        QElapsedTimer timer;
        timer.start();
        {
            AsyncFrameSource async(std::make_unique<SlowSource>(200, 30), 0);
            async.request(100, 1);
            QTest::qWait(10);  // the worker is now inside a decode
        }
        QVERIFY2(timer.elapsed() < 1000, qPrintable(QStringLiteral("took %1 ms").arg(timer.elapsed())));
    }

    void async_serves_exact_frames_of_a_real_video() {
        if (!videoDecodingAvailable()) QSKIP("built without FFmpeg");
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()) {
            QSKIP("the ffmpeg executable is not in PATH");
        }
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = makeRampVideo(dir.path());
        QVERIFY2(!path.isEmpty(), "ffmpeg could not make the test video");

        QString error;
        AsyncFrameSource async(openFrameSource(path, &error), 0);
        QCOMPARE(async.frameCount(), 30);

        for (const int n : {17, 3, 29, 12}) {  // forward jump, back, to the end, back again
            async.request(n, 1);
            QTRY_VERIFY_WITH_TIMEOUT(!async.exactFrame(n).isNull(), 3000);
            const int gray = qGray(async.exactFrame(n).pixel(32, 24));
            QVERIFY2(qAbs(gray - expectedGray(n)) <= 6,
                     qPrintable(QStringLiteral("frame %1: grey %2, expected about %3")
                                    .arg(n).arg(gray).arg(expectedGray(n))));
        }
    }

    void session_shows_a_picture_at_once_and_the_right_one_soon() {
        if (!videoDecodingAvailable()) QSKIP("built without FFmpeg");
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()) {
            QSKIP("the ffmpeg executable is not in PATH");
        }
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = makeRampVideo(dir.path());
        QVERIFY2(!path.isEmpty(), "ffmpeg could not make the test video");

        Session session;
        const QString id = session.importFile(path);
        QVERIFY(session.activateSource(id));
        QVERIFY(!session.currentImage().isNull());  // the first frame is there as soon as it is opened

        session.playback().seek(25);
        QVERIFY(!session.currentImage().isNull());  // never blank while frame 25 is being decoded
        const auto grayNow = [&session] { return qGray(session.currentImage().pixel(32, 24)); };
        QTRY_VERIFY_WITH_TIMEOUT(qAbs(grayNow() - expectedGray(25)) <= 6, 3000);
    }

    // Opt-in: FR_SEQ=/path/shot.0001.png build/tests/test_runtime benchmark_image_sequence_playback
    // Plays a real image sequence twice round its loop and counts, for every frame the playhead
    // shows, whether the picture on screen was that exact frame. Prints frames per second.
    void benchmark_image_sequence_playback() {
        const QString first = qEnvironmentVariable("FR_SEQ");
        if (first.isEmpty()) QSKIP("set FR_SEQ to the first file of an image sequence");
        Session session;
        session.setCacheMegabytes(qEnvironmentVariableIntValue("FR_CACHE_MB") > 0
                                      ? qEnvironmentVariableIntValue("FR_CACHE_MB") : 4096);
        const QString id = session.importSequence(first);
        QVERIFY(!id.isEmpty());
        QVERIFY(session.activateSource(id));
        PlaybackController& playback = session.playback();
        int shown = 0;
        int exact = 0;
        connect(&playback, &PlaybackController::frameChanged, &session, [&] {
            session.currentImage();  // what the viewer does on every frame
            ++shown;
            if (session.currentImageIsExact()) ++exact;
        });
        for (int lap = 1; lap <= 2; ++lap) {
            playback.seek(0);
            shown = exact = 0;
            QElapsedTimer timer;
            timer.start();
            playback.play();
            QTest::qWait(5000);
            playback.pause();
            qInfo("lap %d: %d frames in %.1f s = %.1f fps, %d of them exact", lap, shown, timer.elapsed() / 1000.0,
                  shown * 1000.0 / timer.elapsed(), exact);
            QCOMPARE(exact, shown);  // never a stand-in frame on screen
        }
    }

    // Opt-in: FR_BENCH=1 build/tests/test_runtime benchmark_1080p_blocking_versus_async
    void benchmark_1080p_blocking_versus_async() {
        if (!qEnvironmentVariableIsSet("FR_BENCH")) QSKIP("set FR_BENCH=1 to run the benchmark");
        if (!videoDecodingAvailable()) QSKIP("built without FFmpeg");
        const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
        if (ffmpeg.isEmpty()) QSKIP("the ffmpeg executable is not in PATH");

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("bench.mp4"));
        QProcess make;
        make.start(ffmpeg, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-f"),
                            QStringLiteral("lavfi"), QStringLiteral("-i"),
                            QStringLiteral("testsrc2=size=1920x1080:rate=30:duration=6"),
                            QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-preset"),
                            QStringLiteral("veryfast"), QStringLiteral("-crf"), QStringLiteral("23"),
                            QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"), QStringLiteral("-y"), path});
        QVERIFY(make.waitForFinished(120000));
        QVERIFY2(make.exitCode() == 0, make.readAllStandardError().constData());

        const int jumps[] = {150, 12, 99, 3, 170, 60, 140, 25, 110, 80, 5, 160, 45, 130, 70};
        const int jumpCount = static_cast<int>(sizeof(jumps) / sizeof(jumps[0]));

        // 1. The decoder used directly, the way the UI thread used to call it.
        QString error;
        std::unique_ptr<FrameSource> direct = openFrameSource(path, &error);
        QVERIFY2(direct, qPrintable(error));
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < 90; ++i) direct->frame(i);
        const double sequentialMs = timer.elapsed() / 90.0;
        timer.restart();
        for (const int j : jumps) direct->frame(j);
        const double jumpMs = timer.elapsed() / static_cast<double>(jumpCount);
        direct.reset();

        // 2. The same file through AsyncFrameSource, played at 30 fps.
        AsyncFrameSource async(openFrameSource(path, &error), 0);
        QElapsedTimer clock;
        clock.start();
        int exactHits = 0;
        qint64 worstCallUs = 0;
        const int ticks = 150;
        for (int i = 0; i < ticks; ++i) {
            while (clock.nsecsElapsed() < static_cast<qint64>(i * 1e9 / 30.0)) QTest::qWait(1);
            QElapsedTimer call;
            call.start();
            async.request(i, 1);
            const bool exact = !async.exactFrame(i).isNull();
            if (exact) async.nearestFrame(i);
            worstCallUs = std::max(worstCallUs, call.nsecsElapsed() / 1000);
            if (exact) ++exactHits;
        }

        // 3. How long a jump takes to show the exact frame.
        double latencySum = 0.0;
        double latencyMax = 0.0;
        for (const int j : jumps) {
            QElapsedTimer wait;
            wait.start();
            async.request(j, 1);
            while (async.exactFrame(j).isNull() && wait.elapsed() < 3000) QTest::qWait(1);
            const double ms = static_cast<double>(wait.nsecsElapsed()) / 1e6;
            latencySum += ms;
            latencyMax = std::max(latencyMax, ms);
        }

        qInfo().noquote() << QStringLiteral(
                                 "1080p H.264, %1 frames, cache limit %2 frames\n"
                                 "  blocking decoder:  %3 ms/frame sequential, %4 ms per jump (UI thread waits this long)\n"
                                 "  async, 30 fps play: exact frame on time for %5 of %6 ticks; worst request+lookup %7 us\n"
                                 "  async, jumps:       exact frame after %8 ms on average, %9 ms at worst")
                                 .arg(async.frameCount()).arg(async.cacheLimit())
                                 .arg(sequentialMs, 0, 'f', 1).arg(jumpMs, 0, 'f', 1)
                                 .arg(exactHits).arg(ticks).arg(worstCallUs)
                                 .arg(latencySum / jumpCount, 0, 'f', 1).arg(latencyMax, 0, 'f', 1);
    }

    // --- bookmarks on a Session with a real 30-frame video -------------------------------------
    void frame_bookmarks_toggle_navigate_and_undo() {
        if (!videoDecodingAvailable()) QSKIP("built without FFmpeg");
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()) {
            QSKIP("the ffmpeg executable is not in PATH");
        }
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Session session;
        const QString id = openRamp(session, dir.path());
        QVERIFY2(!id.isEmpty(), "could not open the test video");
        QSignalSpy changed(&session, &Session::bookmarksChanged);
        const auto frames = [&session, &id] { return session.project().findSource(id)->frameBookmarks; };

        session.playback().seek(12);
        session.toggleFrameBookmark();
        session.playback().seek(5);
        session.toggleFrameBookmark();
        QCOMPARE(frames(), (QVector<int>{5, 12}));  // kept sorted

        session.toggleFrameBookmark();  // at 5 again: removes it
        QCOMPARE(frames(), (QVector<int>{12}));
        session.undoStack().undo();
        QCOMPARE(frames(), (QVector<int>{5, 12}));
        session.undoStack().undo();
        QCOMPARE(frames(), (QVector<int>{12}));
        session.undoStack().redo();
        QCOMPARE(frames(), (QVector<int>{5, 12}));

        // The places to visit are 5, 12 and the start of this range bookmark: 20.
        session.playback().setRange(20, 24);
        session.toggleRangeBookmark();
        session.playback().seek(0);
        const auto here = [&session] { return session.playback().frame(); };
        QVERIFY(session.goToAdjacentBookmark(1));
        QCOMPARE(here(), 5);
        QVERIFY(session.goToAdjacentBookmark(1));
        QCOMPARE(here(), 12);
        QVERIFY(session.goToAdjacentBookmark(1));
        QCOMPARE(here(), 20);
        QVERIFY(session.goToAdjacentBookmark(1));
        QCOMPARE(here(), 5);  // wrapped
        QVERIFY(session.goToAdjacentBookmark(-1));
        QCOMPARE(here(), 20);  // wrapped backwards

        session.removeFrameBookmark(5);
        QCOMPARE(frames(), (QVector<int>{12}));
        const int before = changed.count();
        session.removeFrameBookmark(99);  // not a bookmark: nothing happens
        QCOMPARE(changed.count(), before);

        session.clearBookmarks();
        QVERIFY(frames().isEmpty());
        QVERIFY(session.project().findSource(id)->rangeBookmarks.isEmpty());
        QVERIFY(!session.goToAdjacentBookmark(1));  // none left
        session.undoStack().undo();
        QCOMPARE(frames(), (QVector<int>{12}));
        QCOMPARE(session.project().findSource(id)->rangeBookmarks.size(), 1);
    }

    void range_bookmarks_follow_the_playback_range() {
        if (!videoDecodingAvailable()) QSKIP("built without FFmpeg");
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()) {
            QSKIP("the ffmpeg executable is not in PATH");
        }
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Session session;
        const QString id = openRamp(session, dir.path());
        QVERIFY2(!id.isEmpty(), "could not open the test video");
        const auto ranges = [&session, &id] { return session.project().findSource(id)->rangeBookmarks; };

        session.playback().setRange(5, 9);
        session.toggleRangeBookmark();
        QCOMPARE(ranges(), (QVector<RangeBookmark>{RangeBookmark{5, 9}}));
        session.toggleRangeBookmark();  // the same range again removes it
        QVERIFY(ranges().isEmpty());
        session.undoStack().undo();
        QCOMPARE(ranges().size(), 1);

        session.playback().setRange(2, 3);
        session.toggleRangeBookmark();
        QCOMPARE(ranges(), (QVector<RangeBookmark>{RangeBookmark{2, 3}, RangeBookmark{5, 9}}));  // sorted
        session.removeRangeBookmark(RangeBookmark{2, 3});
        QCOMPARE(ranges(), (QVector<RangeBookmark>{RangeBookmark{5, 9}}));

        // Setting the playback range from the playhead.
        const PlaybackController& playback = session.playback();
        session.resetRange();
        QCOMPARE(playback.rangeStart(), 0);
        QCOMPARE(playback.rangeEnd(), 29);
        session.playback().seek(10);
        session.setRangeStartHere();
        QCOMPARE(playback.rangeStart(), 10);
        QCOMPARE(playback.rangeEnd(), 29);
        session.playback().seek(15);
        session.setRangeEndHere();
        QCOMPARE(playback.rangeStart(), 10);
        QCOMPARE(playback.rangeEnd(), 15);
        session.playback().seek(3);  // before the start: the range collapses onto the playhead
        session.setRangeEndHere();
        QCOMPARE(playback.rangeStart(), 3);
        QCOMPARE(playback.rangeEnd(), 3);
        session.resetRange();
        QCOMPARE(playback.rangeEnd(), 29);
    }

    void bookmarks_without_an_active_source_are_ignored() {
        Session session;
        QSignalSpy changed(&session, &Session::bookmarksChanged);
        session.toggleFrameBookmark();
        session.toggleRangeBookmark();
        session.removeFrameBookmark(3);
        session.removeRangeBookmark(RangeBookmark{1, 2});
        session.clearBookmarks();
        session.setRangeStartHere();
        QVERIFY(!session.goToAdjacentBookmark(1));
        QCOMPARE(changed.count(), 0);
        QVERIFY(!session.undoStack().canUndo());
    }

    void stepping_pauses_playback() {
        PlaybackController playback;
        playback.configure(10, 24.0);
        playback.play();
        QVERIFY(playback.isPlaying());
        playback.step(1);
        QVERIFY(!playback.isPlaying());
    }

    void video_decodes_frames_by_index() {
        if (!videoDecodingAvailable()) QSKIP("built without FFmpeg");
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()) {
            QSKIP("the ffmpeg executable is not in PATH");
        }
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = makeRampVideo(dir.path());
        QVERIFY2(!path.isEmpty(), "ffmpeg could not make the test video");

        QString error;
        std::unique_ptr<FrameSource> source = openFrameSource(path, &error);
        QVERIFY2(source, qPrintable(error));
        QCOMPARE(source->fps(), 10.0);
        QCOMPARE(source->frameCount(), 30);
        QCOMPARE(source->size(), QSize(64, 48));

        // Forward steps, a long jump, a jump back, a repeat, and the first frame again.
        const int order[] = {0, 1, 2, 29, 5, 6, 17, 17, 0};
        for (const int n : order) {
            const QImage image = source->frame(n);
            QVERIFY2(!image.isNull(), qPrintable(QStringLiteral("frame %1 is null").arg(n)));
            const int gray = qGray(image.pixel(32, 24));
            QVERIFY2(qAbs(gray - expectedGray(n)) <= 6,
                     qPrintable(QStringLiteral("frame %1: grey %2, expected about %3")
                                    .arg(n).arg(gray).arg(expectedGray(n))));
        }
    }
};

QTEST_MAIN(RuntimeTests)
#include "test_runtime.moc"
