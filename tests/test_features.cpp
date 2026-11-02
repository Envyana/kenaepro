// Tests for image sequences, timelines, two viewers, drawing helpers, numbering, export, audio
// and preferences. Everything runs without a window; image sequences are generated on the fly.
#include "core/AudioController.h"
#include "core/Exporter.h"
#include "core/FrameNumbering.h"
#include "core/Preferences.h"
#include "core/ProjectModel.h"
#include "core/Session.h"
#include "core/StrokeRenderer.h"
#include "media/AudioMix.h"
#include "media/AudioTrack.h"
#include "media/ClipSequenceSource.h"
#include "media/FrameSource.h"
#include "media/ImageSequence.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>
#include <memory>

using namespace fr;

namespace {

constexpr int kW = 32;
constexpr int kH = 24;

// Writes shot.NNNN.png for every number in `numbers`; the red channel of frame n is 8 * n.
QString writeSequence(const QString& dir, const QString& prefix, const QVector<int>& numbers, int digits = 4) {
    QDir().mkpath(dir);
    for (const int n : numbers) {
        QImage image(kW, kH, QImage::Format_RGB32);
        image.fill(qRgb(8 * n, 0, 0));
        const QString name = prefix + QString::number(n).rightJustified(digits, QLatin1Char('0')) +
                             QStringLiteral(".png");
        if (!image.save(QDir(dir).filePath(name))) return {};
    }
    return QDir(dir).filePath(prefix + QString::number(numbers.first()).rightJustified(digits, QLatin1Char('0')) +
                              QStringLiteral(".png"));
}

QVector<int> range(int first, int last) {
    QVector<int> v;
    for (int i = first; i <= last; ++i) v.push_back(i);
    return v;
}

int redOf(const QImage& image) {
    return qRed(image.pixel(0, 0));
}

Stroke lineStroke(const QColor& color, double y, double width = 4.0) {
    Stroke s;
    s.type = Stroke::Type::Pencil;
    s.color = color;
    s.width = width;
    s.points = {QPointF(0.0, y), QPointF(1.0, y)};
    return s;
}

}  // namespace

class FeaturesTest : public QObject {
    Q_OBJECT

private slots:
    // --- image sequences -------------------------------------------------------------------
    void parsePatternPath();
    void sequenceFromFileDetectsPadding();
    void scanSkipsGaps();
    void sequenceSourceDecodes();
    void factoryOpensSequences();
    void findsSequencesInFolder();

    // --- clips -----------------------------------------------------------------------------
    void clipSequenceConcatenates();

    // --- numbering -------------------------------------------------------------------------
    void timecode();
    void frameLabels();

    // --- strokes ---------------------------------------------------------------------------
    void strokeDistanceAndHit();
    void strokeTranslationStaysInside();
    void smoothingKeepsEnds();
    void strokePaintsPixels();

    // --- model -----------------------------------------------------------------------------
    void cutRangeClamps();
    void holdShowsEarlierStrokes();

    // --- session ---------------------------------------------------------------------------
    void importSequenceAsSource();
    void timelinePlaysClipsBackToBack();
    void cutShortensTimelineClip();
    void secondViewerHasOffset();
    void strokesUndoAndHold();
    void bookmarksAndFilter();
    void numberingInSession();
    void clipboardCopiesStrokes();
    void eraserRemovesTouchedStrokes();
    void treeEditing();
    void viewSettingsSurviveSave();

    // --- export ----------------------------------------------------------------------------
    void exportFrameSelection();
    void exportNames();
    void exportValidation();
    void exportsImageSequenceWithDrawings();
    void exportCanBeCancelled();
    void exportsVideoWithFfmpeg();

    // --- audio and preferences -------------------------------------------------------------
    void audioTrackDecodesSine();
    void audioMixPlacesSegments();
    void timelineHasSound();
    void preferencesRoundTrip();
};

// --- image sequences -----------------------------------------------------------------------

void FeaturesTest::parsePatternPath() {
    QVERIFY(isSequencePath(QStringLiteral("/d/shot.%04d.png")));
    QVERIFY(isSequencePath(QStringLiteral("/d/shot.%d.png")));
    QVERIFY(!isSequencePath(QStringLiteral("/d/shot.png")));
    QVERIFY(!isSequencePath(QStringLiteral("/d/shot.%04d.txt.png")));

    const auto pattern = parseSequencePath(QStringLiteral("/d/shot.%04d.png"));
    QVERIFY(pattern.has_value());
    QCOMPARE(pattern->directory, QStringLiteral("/d"));
    QCOMPARE(pattern->prefix, QStringLiteral("shot."));
    QCOMPARE(pattern->suffix, QStringLiteral(".png"));
    QCOMPARE(pattern->digits, 4);
    QCOMPARE(pattern->pathFor(7), QStringLiteral("/d/shot.0007.png"));
    QCOMPARE(pattern->pattern(), QStringLiteral("/d/shot.%04d.png"));

    const auto unpadded = parseSequencePath(QStringLiteral("/d/a_%d.jpg"));
    QVERIFY(unpadded.has_value());
    QCOMPARE(unpadded->digits, 0);
    QCOMPARE(unpadded->pathFor(12), QStringLiteral("/d/a_12.jpg"));
}

void FeaturesTest::sequenceFromFileDetectsPadding() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString padded = writeSequence(dir.filePath(QStringLiteral("padded")), QStringLiteral("shot."), range(1, 3));
    const auto a = sequenceFromFile(padded);
    QVERIFY(a.has_value());
    QCOMPARE(a->digits, 4);
    QCOMPARE(a->prefix, QStringLiteral("shot."));

    // Numbers of different widths mean the numbering is not padded.
    const QString plain = writeSequence(dir.filePath(QStringLiteral("plain")), QStringLiteral("p"), {9, 10, 11}, 1);
    const auto b = sequenceFromFile(plain);
    QVERIFY(b.has_value());
    QCOMPARE(b->digits, 0);

    QVERIFY(!sequenceFromFile(dir.filePath(QStringLiteral("nodigits.png"))).has_value());
}

void FeaturesTest::scanSkipsGaps() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString first = writeSequence(dir.path(), QStringLiteral("g."), {1, 2, 4, 5});
    QVERIFY(!first.isEmpty());
    const auto pattern = sequenceFromFile(first);
    QVERIFY(pattern.has_value());
    QCOMPARE(scanSequence(*pattern), (QVector<int>{1, 2, 4, 5}));
}

void FeaturesTest::sequenceSourceDecodes() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString first = writeSequence(dir.path(), QStringLiteral("s."), {1, 2, 4, 5});
    const auto pattern = sequenceFromFile(first);
    QVERIFY(pattern.has_value());

    ImageSequenceSource source;
    QString error;
    QVERIFY2(source.open(pattern->pattern(), &error), qPrintable(error));
    QCOMPARE(source.frameCount(), 4);
    QCOMPARE(source.size(), QSize(kW, kH));
    QCOMPARE(source.fps(), 0.0);
    QCOMPARE(source.frameNumbers(), (QVector<int>{1, 2, 4, 5}));
    QCOMPARE(redOf(source.frame(0)), 8);
    QCOMPARE(redOf(source.frame(2)), 32);  // index 2 is file number 4
    QVERIFY(source.frame(4).isNull());

    ImageSequenceSource missing;
    QVERIFY(!missing.open(dir.filePath(QStringLiteral("none.%04d.png")), &error));
    QVERIFY(!error.isEmpty());
}

void FeaturesTest::factoryOpensSequences() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString first = writeSequence(dir.path(), QStringLiteral("f."), range(1, 6));
    const auto pattern = sequenceFromFile(first);
    QVERIFY(pattern.has_value());
    QString error;
    const auto source = openFrameSource(pattern->pattern(), &error);
    QVERIFY2(source != nullptr, qPrintable(error));
    QCOMPARE(source->frameCount(), 6);
    QCOMPARE(source->frameNumbers().size(), 6);
}

void FeaturesTest::findsSequencesInFolder() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(!writeSequence(dir.path(), QStringLiteral("one."), range(1, 3)).isEmpty());
    QVERIFY(!writeSequence(dir.path(), QStringLiteral("two_"), range(10, 14)).isEmpty());
    QImage lone(kW, kH, QImage::Format_RGB32);
    lone.fill(Qt::blue);
    QVERIFY(lone.save(dir.filePath(QStringLiteral("single.png"))));

    const QVector<SequenceGroup> groups = findSequencesInFolder(dir.path());
    QCOMPARE(groups.size(), 2);
    int total = 0;
    for (const SequenceGroup& g : groups) total += g.numbers.size();
    QCOMPARE(total, 8);
}

// --- clips ---------------------------------------------------------------------------------

void FeaturesTest::clipSequenceConcatenates() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto open = [&](const QString& sub, const QVector<int>& numbers) {
        const QString first = writeSequence(dir.filePath(sub), QStringLiteral("c."), numbers);
        auto source = std::make_unique<ImageSequenceSource>();
        const auto pattern = sequenceFromFile(first);
        const bool ok = pattern && source->open(pattern->pattern(), nullptr);
        return ok ? std::unique_ptr<FrameSource>(std::move(source)) : std::unique_ptr<FrameSource>();
    };

    std::vector<ClipSequenceSource::Entry> entries;
    ClipSequenceSource::Entry a;
    a.source = open(QStringLiteral("a"), range(1, 5));
    a.first = 1;
    a.count = 3;  // file numbers 2, 3, 4
    ClipSequenceSource::Entry b;
    b.source = open(QStringLiteral("b"), range(10, 13));
    b.first = 0;
    b.count = 4;
    QVERIFY(a.source && b.source);
    entries.push_back(std::move(a));
    entries.push_back(std::move(b));

    ClipSequenceSource clips(std::move(entries));
    QCOMPARE(clips.frameCount(), 7);
    QCOMPARE(clips.size(), QSize(kW, kH));
    QCOMPARE(redOf(clips.frame(0)), 16);   // number 2
    QCOMPARE(redOf(clips.frame(2)), 32);   // number 4
    QCOMPARE(redOf(clips.frame(3)), 80);   // number 10, first frame of the second run
    QCOMPARE(redOf(clips.frame(6)), 104);  // number 13
    QVERIFY(clips.frame(7).isNull());
    QCOMPARE(clips.frameNumbers(), (QVector<int>{2, 3, 4, 10, 11, 12, 13}));
}

// --- numbering -----------------------------------------------------------------------------

void FeaturesTest::timecode() {
    QCOMPARE(formatTimecode(0, 24.0), QStringLiteral("00:00:00:00"));
    QCOMPARE(formatTimecode(30, 24.0), QStringLiteral("00:00:01:06"));
    QCOMPARE(formatTimecode(25 * 3600 + 25 * 61 + 3, 25.0), QStringLiteral("01:01:01:03"));
    QCOMPARE(formatTimecode(-5, 24.0), QStringLiteral("00:00:00:00"));
    QCOMPARE(formatTimecode(24, 23.976), QStringLiteral("00:00:01:00"));  // the rate is rounded

    QCOMPARE(formatDuration(0, 24.0), QStringLiteral("00:00.000"));
    QCOMPARE(formatDuration(30, 24.0), QStringLiteral("00:01.250"));
    QCOMPARE(formatDuration(24 * 83, 24.0), QStringLiteral("01:23.000"));
    QCOMPARE(formatDuration(25 * 3725, 25.0), QStringLiteral("1:02:05.000"));
    QCOMPARE(formatDuration(24, 23.976), QStringLiteral("00:01.001"));  // the exact rate
    QCOMPARE(formatDuration(-5, 24.0), QStringLiteral("00:00.000"));
}

void FeaturesTest::frameLabels() {
    FrameLabelInput in;
    in.frame = 4;
    in.frameCount = 100;
    in.rangeStart = 10;
    in.rangeEnd = 19;
    in.fps = 25.0;
    QCOMPARE(formatFrameLabel(NumberingMode::Global, in), QStringLiteral("5 / 100"));
    in.tcOffset = 100;
    QCOMPARE(formatFrameLabel(NumberingMode::Global, in), QStringLiteral("105 / 200"));
    QCOMPARE(formatFrameLabel(NumberingMode::Timecode, in), QStringLiteral("00:00:04:04"));
    in.tcOffset = 0;
    QCOMPARE(formatFrameLabel(NumberingMode::Image, in), QStringLiteral("5 / 100"));  // no file number
    in.imageNumber = 1007;
    QCOMPARE(formatFrameLabel(NumberingMode::Image, in), QStringLiteral("#1007"));
    in.frame = 12;
    QCOMPARE(formatFrameLabel(NumberingMode::Range, in), QStringLiteral("3 / 10"));
    in.frameCount = 0;
    QVERIFY(formatFrameLabel(NumberingMode::Global, in).isEmpty());
}

// --- strokes -------------------------------------------------------------------------------

void FeaturesTest::strokeDistanceAndHit() {
    const Stroke line = lineStroke(Qt::green, 0.5);
    // 0.1 of the height below the line, however wide the picture is.
    QVERIFY(qAbs(distanceToStroke(line, QPointF(0.5, 0.6), 2.0) - 0.1) < 1e-9);
    QVERIFY(distanceToStroke(line, QPointF(0.3, 0.5), 2.0) < 1e-9);
    // Beyond the end of a segment the distance is to its end point (horizontal gap counts the aspect).
    QVERIFY(qAbs(distanceToStroke(line, QPointF(1.0, 0.5), 2.0)) < 1e-9);

    Stroke rect;
    rect.type = Stroke::Type::Rectangle;
    rect.points = {QPointF(0.2, 0.2), QPointF(0.4, 0.4)};
    QVector<Stroke> strokes{line, rect};
    QCOMPARE(strokeAt(strokes, QPointF(0.5, 0.505), 1.0, 0.02), 0);
    QCOMPARE(strokeAt(strokes, QPointF(0.2, 0.3), 1.0, 0.02), 1);  // on the rectangle's edge
    QCOMPARE(strokeAt(strokes, QPointF(0.9, 0.1), 1.0, 0.02), -1);
}

void FeaturesTest::strokeTranslationStaysInside() {
    Stroke s;
    s.type = Stroke::Type::Rectangle;
    s.points = {QPointF(0.6, 0.2), QPointF(0.8, 0.4)};
    const Stroke moved = translatedStroke(s, QPointF(0.5, -0.5));
    // The delta is shortened, the shape keeps its size: x stops at 1.0, y at 0.0.
    QVERIFY(qAbs(moved.points[0].x() - 0.8) < 1e-9);
    QVERIFY(qAbs(moved.points[1].x() - 1.0) < 1e-9);
    QVERIFY(qAbs(moved.points[0].y() - 0.0) < 1e-9);
    QVERIFY(qAbs(moved.points[1].y() - 0.2) < 1e-9);

    const Stroke same = translatedStroke(s, QPointF(0.0, 0.0));
    QCOMPARE(same.points, s.points);
}

void FeaturesTest::smoothingKeepsEnds() {
    const QVector<QPointF> line{QPointF(0.0, 0.0), QPointF(0.5, 0.5), QPointF(1.0, 0.0)};
    QCOMPARE(smoothPoints(line, 0), line);
    const QVector<QPointF> once = smoothPoints(line, 1);
    QCOMPARE(once.size(), 6);  // 2 end points + 2 per segment
    QCOMPARE(once.first(), line.first());
    QCOMPARE(once.last(), line.last());
    QCOMPARE(smoothPoints(line, 2).size(), 12);
    // Two points cannot be smoothed.
    QCOMPARE(smoothPoints({QPointF(0, 0), QPointF(1, 1)}, 2).size(), 2);
}

void FeaturesTest::strokePaintsPixels() {
    QImage image(100, 100, QImage::Format_ARGB32);
    image.fill(Qt::black);
    {
        QPainter painter(&image);
        paintStroke(painter, lineStroke(QColor(0, 255, 0), 0.5, 10.0), QRectF(0, 0, 100, 100), 100.0);
    }
    QVERIFY(qGreen(image.pixel(50, 50)) > 200);
    QCOMPARE(qGreen(image.pixel(50, 10)), 0);

    // A filled rectangle fills its inside; an unfilled one leaves it black.
    Stroke rect;
    rect.type = Stroke::Type::Rectangle;
    rect.color = QColor(0, 0, 255);
    rect.width = 2.0;
    rect.points = {QPointF(0.2, 0.2), QPointF(0.8, 0.8)};
    image.fill(Qt::black);
    {
        QPainter painter(&image);
        paintStroke(painter, rect, QRectF(0, 0, 100, 100), 100.0);
    }
    QCOMPARE(qBlue(image.pixel(50, 50)), 0);
    rect.fillOpacity = 1.0;
    image.fill(Qt::black);
    {
        QPainter painter(&image);
        paintStroke(painter, rect, QRectF(0, 0, 100, 100), 100.0);
    }
    QVERIFY(qBlue(image.pixel(50, 50)) > 200);
}

// --- model ---------------------------------------------------------------------------------

void FeaturesTest::cutRangeClamps() {
    Source s;
    QCOMPARE(s.cutRange(10), std::make_pair(0, 9));
    s.cutInFrame = 3;
    s.cutOutFrame = 8;
    QCOMPARE(s.cutRange(10), std::make_pair(3, 8));
    s.cutOutFrame = 50;
    QCOMPARE(s.cutRange(10), std::make_pair(3, 9));
    s.cutInFrame = 20;
    const auto cut = s.cutRange(10);
    QVERIFY(cut.first <= cut.second);
    QVERIFY(cut.first >= 0 && cut.second <= 9);
}

void FeaturesTest::holdShowsEarlierStrokes() {
    Source s;
    s.annotationForEdit(5).strokes.push_back(lineStroke(Qt::red, 0.3));
    s.annotationForEdit(12).strokes.push_back(lineStroke(Qt::red, 0.4));
    s.annotationForEdit(12).strokes.push_back(lineStroke(Qt::red, 0.6));

    QCOMPARE(s.visibleStrokes(8).size(), 0);  // no hold: only the frame's own
    QCOMPARE(s.visibleStrokes(12).size(), 2);
    s.holdAnnotations = true;
    QCOMPARE(s.visibleStrokes(4).size(), 0);
    QCOMPARE(s.visibleStrokes(5).size(), 1);
    QCOMPARE(s.visibleStrokes(8).size(), 1);
    QCOMPARE(s.visibleStrokes(12).size(), 2);
    QCOMPARE(s.visibleStrokes(30).size(), 2);
}

// --- session -------------------------------------------------------------------------------

namespace {

struct Seqs {
    QString a;  // 10 frames, numbers 1..10, red = 8 * number
    QString b;  // 6 frames, numbers 21..26
};

Seqs makeSeqs(Session& session, const QString& dir, QString* idA, QString* idB) {
    Seqs s;
    s.a = writeSequence(QDir(dir).filePath(QStringLiteral("a")), QStringLiteral("a."), range(1, 10));
    s.b = writeSequence(QDir(dir).filePath(QStringLiteral("b")), QStringLiteral("b."), range(21, 26));
    *idA = session.importSequence(s.a);
    *idB = session.importSequence(s.b);
    return s;
}

}  // namespace

void FeaturesTest::importSequenceAsSource() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Session session;
    QString idA, idB;
    makeSeqs(session, dir.path(), &idA, &idB);
    QVERIFY(!idA.isEmpty());
    QVERIFY(!idB.isEmpty());

    const Source* a = session.project().findSource(idA);
    QVERIFY(a);
    QVERIFY(isSequencePath(a->path));

    QString error;
    QVERIFY2(session.activateSource(idA, &error), qPrintable(error));
    QCOMPARE(session.playback().frameCount(), 10);
    QCOMPARE(session.viewerTargetKind(), QStringLiteral("source"));
    QCOMPARE(session.activeSourceId(), idA);
    session.playback().seek(3);
    QTRY_VERIFY(!session.currentImage().isNull());
    QTRY_COMPARE(redOf(session.currentImage()), 32);

    // Importing a folder (not its subfolders) makes one source per sequence and one per other
    // image; the frames of a sequence are not imported a second time as single images.
    const QString flat = dir.filePath(QStringLiteral("flat"));
    QVERIFY(!writeSequence(flat, QStringLiteral("x."), range(1, 4)).isEmpty());
    QVERIFY(!writeSequence(flat, QStringLiteral("y_"), range(7, 9)).isEmpty());
    QImage lone(kW, kH, QImage::Format_RGB32);
    lone.fill(Qt::blue);
    QVERIFY(lone.save(QDir(flat).filePath(QStringLiteral("poster.png"))));
    Session other;
    QString importError;
    const QStringList ids = other.importFolder(flat, &importError);
    QVERIFY2(importError.isEmpty(), qPrintable(importError));
    QCOMPARE(ids.size(), 3);
    QCOMPARE(other.project().sources.size(), 3);
    int sequences = 0;
    for (const Source& source : other.project().sources) sequences += isSequencePath(source.path) ? 1 : 0;
    QCOMPARE(sequences, 2);
    QVERIFY(other.importFolder(dir.filePath(QStringLiteral("missing")), &importError).isEmpty());
    QVERIFY(!importError.isEmpty());
}

void FeaturesTest::timelinePlaysClipsBackToBack() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Session session;
    QString idA, idB;
    makeSeqs(session, dir.path(), &idA, &idB);

    const QString timeline = session.addTimeline(QStringLiteral("T"));
    QVERIFY(!timeline.isEmpty());
    session.addClipToTimeline(timeline, idA);
    session.addClipToTimeline(timeline, idB);
    QCOMPARE(session.project().findTimeline(timeline)->clips.size(), 2);

    QString error;
    QVERIFY2(session.activateTimeline(timeline, &error), qPrintable(error));
    QCOMPARE(session.playback().frameCount(), 16);
    QCOMPARE(session.viewerTargetKind(), QStringLiteral("timeline"));

    session.playback().seek(12);
    QCOMPARE(session.activeSourceId(), idB);
    const auto where = session.location();
    QVERIFY(where.has_value());
    QCOMPARE(where->sourceId, idB);
    QCOMPARE(where->frame, 2);
    session.playback().seek(4);
    QCOMPARE(session.activeSourceId(), idA);

    // Editing the timeline is undoable and keeps the shown length in step.
    session.moveClipInTimeline(timeline, 1, 0);
    QCOMPARE(session.project().findTimeline(timeline)->clips.first().sourceId, idB);
    session.undoStack().undo();
    QCOMPARE(session.project().findTimeline(timeline)->clips.first().sourceId, idA);
    session.removeClipFromTimeline(timeline, 0);
    QCOMPARE(session.playback().frameCount(), 6);
}

void FeaturesTest::cutShortensTimelineClip() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Session session;
    QString idA, idB;
    makeSeqs(session, dir.path(), &idA, &idB);

    QVERIFY(session.activateSource(idA));
    session.playback().seek(3);
    session.setCutInHere();
    session.playback().seek(8);
    session.setCutOutHere();
    const Source* a = session.project().findSource(idA);
    QCOMPARE(a->cutInFrame, 3);
    QCOMPARE(a->cutOutFrame, 8);

    const QString timeline = session.addTimeline(QStringLiteral("T"));
    session.addClipToTimeline(timeline, idA);
    session.addClipToTimeline(timeline, idB);
    QVERIFY(session.activateTimeline(timeline));
    QCOMPARE(session.playback().frameCount(), 6 + 6);  // frames 3..8 of A, then all of B
    session.playback().seek(0);
    QCOMPARE(session.location()->frame, 3);

    QVERIFY(session.activateSource(idA));
    session.clearCut();
    QCOMPARE(session.project().findSource(idA)->cutInFrame, 0);
    QCOMPARE(session.project().findSource(idA)->cutOutFrame, -1);
}

void FeaturesTest::secondViewerHasOffset() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Session session;
    QString idA, idB;
    makeSeqs(session, dir.path(), &idA, &idB);

    QCOMPARE(session.viewerCount(), 1);
    QSignalSpy layoutSpy(&session, &Session::viewerLayoutChanged);
    session.setViewerLayout(QStringLiteral("horizontal"));
    QCOMPARE(session.viewerCount(), 2);
    QCOMPARE(layoutSpy.count(), 1);

    QVERIFY(session.activateSource(idA));  // viewer 0
    session.setActiveViewer(1);
    QCOMPARE(session.activeViewer(), 1);
    QVERIFY(session.activateSource(idB));  // viewer 1
    QCOMPARE(session.viewerTargetId(0), idA);
    QCOMPARE(session.viewerTargetId(1), idB);
    QCOMPARE(session.viewerFrameCount(1), 6);

    session.setViewerOffset(1, 2);
    QCOMPARE(session.viewerOffset(1), 2);
    session.setViewerOffset(0, 5);  // the first viewer cannot be shifted
    QCOMPARE(session.viewerOffset(0), 0);

    session.playback().seek(5);
    QCOMPARE(session.location(0)->frame, 5);
    QCOMPARE(session.location(1)->frame, 3);  // playhead - offset
    QCOMPARE(session.location(1)->sourceId, idB);

    // Edits follow the active viewer.
    session.addStroke(lineStroke(Qt::green, 0.5));
    QCOMPARE(session.project().findSource(idB)->annotationAt(3)->strokes.size(), 1);
    QVERIFY(session.project().findSource(idA)->annotationAt(5) == nullptr);

    session.setViewerLayout(QStringLiteral("single"));
    QCOMPARE(session.viewerCount(), 1);
    QCOMPARE(session.activeViewer(), 0);
}

void FeaturesTest::strokesUndoAndHold() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Session session;
    QString idA, idB;
    makeSeqs(session, dir.path(), &idA, &idB);
    QVERIFY(session.activateSource(idA));

    session.playback().seek(2);
    session.addStroke(lineStroke(Qt::green, 0.5));
    QCOMPARE(session.strokesOfFrame().size(), 1);
    session.undoStack().undo();
    QCOMPARE(session.strokesOfFrame().size(), 0);
    session.undoStack().redo();
    QCOMPARE(session.strokesOfFrame().size(), 1);

    QCOMPARE(session.strokesToShow().size(), 1);
    QVERIFY(session.strokesAreEditable());
    session.playback().seek(5);
    QCOMPARE(session.strokesToShow().size(), 0);

    session.setHoldAnnotations(true);
    QCOMPARE(session.strokesToShow().size(), 1);  // held from frame 2
    QVERIFY(!session.strokesAreEditable());
    QCOMPARE(session.strokesOfFrame().size(), 0);
    session.setHoldAnnotations(false);

    session.playback().seek(2);
    session.clearStrokes();
    QCOMPARE(session.strokesOfFrame().size(), 0);
    session.undoStack().undo();
    QCOMPARE(session.strokesOfFrame().size(), 1);

    // Ghost strokes come from the nearest annotated frames around the playhead.
    session.playback().seek(6);
    session.addStroke(lineStroke(Qt::yellow, 0.2));
    session.playback().seek(4);
    QCOMPARE(session.ghostStrokes().size(), 2);

    session.setCurrentNote(QStringLiteral("fix the arm"));
    QCOMPARE(session.currentNote(), QStringLiteral("fix the arm"));
    session.playback().seek(3);
    QCOMPARE(session.currentNote(), QString());

    // Drawn frames 2 and 6, a note on 4: what the slider marks and the Info panel counts.
    QCOMPARE(session.annotatedFrames(), (QVector<int>{2, 4, 6}));
    QCOMPARE(session.viewerFrame(), 3);
    QVERIFY(!session.viewerFrameSize().isEmpty());
}

void FeaturesTest::bookmarksAndFilter() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Session session;
    QString idA, idB;
    makeSeqs(session, dir.path(), &idA, &idB);
    QVERIFY(session.activateSource(idA));

    session.playback().seek(2);
    session.toggleFrameBookmark();
    session.playback().seek(6);
    session.toggleFrameBookmark();
    QCOMPARE(session.project().findSource(idA)->frameBookmarks, (QVector<int>{2, 6}));

    session.playback().seek(2);
    QVERIFY(session.goToAdjacentBookmark(1));
    QCOMPARE(session.playback().frame(), 6);
    QVERIFY(session.goToAdjacentBookmark(1));  // wraps
    QCOMPARE(session.playback().frame(), 2);
    QVERIFY(session.goToAdjacentBookmark(-1));
    QCOMPARE(session.playback().frame(), 6);

    session.playback().setRange(3, 7);
    session.toggleRangeBookmark();
    QCOMPARE(session.project().findSource(idA)->rangeBookmarks.size(), 1);
    QCOMPARE(session.bookmarkEntries().size(), 3);

    // Only frames with drawings: the frame bookmark at 2 qualifies once it has a stroke.
    session.playback().seek(2);
    session.addStroke(lineStroke(Qt::green, 0.5));
    session.setBookmarkFilter(BookmarkFilter::WithDrawings);
    int frameEntries = 0;
    for (const BookmarkEntry& e : session.bookmarkEntries()) frameEntries += e.isRange ? 0 : 1;
    QCOMPARE(frameEntries, 1);
    session.setBookmarkFilter(BookmarkFilter::WithNotes);
    frameEntries = 0;
    for (const BookmarkEntry& e : session.bookmarkEntries()) frameEntries += e.isRange ? 0 : 1;
    QCOMPARE(frameEntries, 0);
    session.setBookmarkFilter(BookmarkFilter::All);

    // Undo goes back one bookmark change at a time.
    session.clearBookmarks();
    QCOMPARE(session.bookmarkEntries().size(), 0);
    session.undoStack().undo();
    QCOMPARE(session.bookmarkEntries().size(), 3);
}

void FeaturesTest::numberingInSession() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Session session;
    QString idA, idB;
    makeSeqs(session, dir.path(), &idA, &idB);
    QVERIFY(session.activateSource(idB));
    session.playback().seek(2);

    QCOMPARE(session.frameLabel(NumberingMode::Global), QStringLiteral("3 / 6"));
    QCOMPARE(session.frameLabel(NumberingMode::Image), QStringLiteral("#23"));
    session.setProjectTcOffset(100);
    QCOMPARE(session.frameLabel(NumberingMode::Global), QStringLiteral("103 / 106"));
    session.setTcOffsetOverride(true, 10);
    QCOMPARE(session.tcOffset(), 10);
    QCOMPARE(session.frameLabel(NumberingMode::Global), QStringLiteral("13 / 16"));
    session.setTcOffsetOverride(false, 0);
    QCOMPARE(session.tcOffset(), 100);

    session.setImageSequenceFrameRate(25.0);
    QCOMPARE(session.project().imageSeqFrameRate, 25.0);
}

void FeaturesTest::clipboardCopiesStrokes() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Session session;
    QString idA, idB;
    makeSeqs(session, dir.path(), &idA, &idB);
    QVERIFY(session.activateSource(idA));

    session.playback().seek(1);
    session.addStroke(lineStroke(Qt::green, 0.5));
    session.addStroke(lineStroke(Qt::yellow, 0.7));
    QVERIFY(!session.hasStrokeClipboard());
    session.copyStrokes();
    QVERIFY(session.hasStrokeClipboard());

    session.playback().seek(4);
    session.pasteStrokes();
    QCOMPARE(session.strokesOfFrame().size(), 2);
    session.undoStack().undo();
    QCOMPARE(session.strokesOfFrame().size(), 0);

    session.playback().seek(1);
    session.cutStrokes();
    QCOMPARE(session.strokesOfFrame().size(), 0);
    session.playback().seek(7);
    session.pasteStrokes();
    QCOMPARE(session.strokesOfFrame().size(), 2);

    // Moving and deleting one stroke.
    session.moveStroke(0, QPointF(0.0, 0.1));
    QVERIFY(qAbs(session.strokesOfFrame().first().points.first().y() - 0.6) < 1e-9);
    session.deleteStroke(1);
    QCOMPARE(session.strokesOfFrame().size(), 1);
}

void FeaturesTest::eraserRemovesTouchedStrokes() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Session session;
    QString idA, idB;
    makeSeqs(session, dir.path(), &idA, &idB);
    QVERIFY(session.activateSource(idA));

    session.playback().seek(1);
    session.addStroke(lineStroke(Qt::green, 0.2));
    session.addStroke(lineStroke(Qt::yellow, 0.8));
    // A vertical sweep through the top line only.
    session.erasePath({QPointF(0.5, 0.0), QPointF(0.5, 0.3)}, 0.03, 1.0);
    const QVector<Stroke> left = session.strokesOfFrame();
    QCOMPARE(left.size(), 1);
    QCOMPARE(left.first().color, QColor(Qt::yellow));
    session.undoStack().undo();
    QCOMPARE(session.strokesOfFrame().size(), 2);
}

void FeaturesTest::treeEditing() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Session session;
    QString idA, idB;
    makeSeqs(session, dir.path(), &idA, &idB);

    QVERIFY(session.renameItem(idA, QStringLiteral("Take A")));
    QCOMPARE(session.project().findSource(idA)->name, QStringLiteral("Take A"));
    QVERIFY(!session.renameItem(QStringLiteral("no-such-id"), QStringLiteral("x")));

    const QString copy = session.duplicateSource(idA);
    QVERIFY(!copy.isEmpty());
    QCOMPARE(session.project().sources.size(), 3);
    QCOMPARE(session.project().findSource(copy)->path, session.project().findSource(idA)->path);

    const QString folder = session.project().addFolder(QStringLiteral("Shots"));
    QVERIFY(!folder.isEmpty());
    QVERIFY(session.moveItem(idB, folder));
    QCOMPARE(session.project().findSource(idB)->parentId, folder);

    const QString timeline = session.addTimeline(QStringLiteral("T"));
    session.addClipToTimeline(timeline, idA);
    QCOMPARE(session.removeItem(idA, false), RemoveStatus::SourceInTimelines);
    QVERIFY(session.project().findSource(idA) != nullptr);
    QCOMPARE(session.removeItem(idA, true), RemoveStatus::Ok);
    QVERIFY(session.project().findSource(idA) == nullptr);
    QCOMPARE(session.project().findTimeline(timeline)->clips.size(), 0);

    QCOMPARE(session.removeItem(folder, false), RemoveStatus::NotEmpty);
    QCOMPARE(session.removeItem(kSourcesFolderId, false), RemoveStatus::BaseFolder);

    session.setDefaultTimeline(timeline);
    QVERIFY(session.project().findTimeline(timeline)->isDefault);

    // A removed source that was on screen empties the viewer.
    QVERIFY(session.activateSource(copy));
    QCOMPARE(session.removeItem(copy, false), RemoveStatus::Ok);
    QVERIFY(session.viewerTargetId().isEmpty());
    QCOMPARE(session.playback().frameCount(), 0);
}

void FeaturesTest::viewSettingsSurviveSave() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Session session;
    QString idA, idB;
    makeSeqs(session, dir.path(), &idA, &idB);
    QVERIFY(session.activateSource(idA));

    session.setPanZoomEnabled(true);
    session.setSourceView(0, 2.5, 0.1, -0.2);
    session.setHoldAnnotations(true);
    session.setAudioDelayMs(120.0);
    session.playback().seek(3);
    session.setCutInHere();
    Stroke filled;
    filled.type = Stroke::Type::Rectangle;
    filled.fillOpacity = 0.5;
    filled.points = {QPointF(0.1, 0.1), QPointF(0.4, 0.4)};
    session.addStroke(filled);
    session.setViewerLayout(QStringLiteral("horizontal"));

    const QString file = dir.filePath(QStringLiteral("p.kpro"));
    QString error;
    QVERIFY2(session.saveProject(file, &error), qPrintable(error));

    Session loaded;
    QVERIFY2(loaded.openProject(file, &error), qPrintable(error));
    const Source* a = loaded.project().findSource(idA);
    QVERIFY(a);
    QVERIFY(a->panZoomEnabled);
    QVERIFY(qAbs(a->zoom - 2.5) < 1e-9);
    QVERIFY(qAbs(a->panX - 0.1) < 1e-9);
    QVERIFY(qAbs(a->panY + 0.2) < 1e-9);
    QVERIFY(a->holdAnnotations);
    QVERIFY(qAbs(a->audioDelayMs - 120.0) < 1e-9);
    QCOMPARE(a->cutInFrame, 3);
    QCOMPARE(a->annotationAt(3)->strokes.size(), 1);
    QVERIFY(qAbs(a->annotationAt(3)->strokes.first().fillOpacity - 0.5) < 1e-9);
    QCOMPARE(loaded.viewerLayout(), QStringLiteral("horizontal"));
    QVERIFY(isSequencePath(a->path));
}

// --- export --------------------------------------------------------------------------------

void FeaturesTest::exportFrameSelection() {
    QCOMPARE(framesToExport(0, 0, -1, {}, ExportOptions::FrameSet::All), QVector<int>());
    QCOMPARE(framesToExport(5, 0, -1, {}, ExportOptions::FrameSet::All), range(0, 4));
    QCOMPARE(framesToExport(10, 2, 4, {}, ExportOptions::FrameSet::Range), range(2, 4));
    QCOMPARE(framesToExport(10, 7, -1, {}, ExportOptions::FrameSet::Range), range(7, 9));
    QCOMPARE(framesToExport(10, 0, 99, {}, ExportOptions::FrameSet::Range), range(0, 9));
    QCOMPARE(framesToExport(10, 0, -1, {1, 5, 12}, ExportOptions::FrameSet::Bookmarks), (QVector<int>{1, 5}));
}

void FeaturesTest::exportNames() {
    ExportOptions o;
    o.baseName = QStringLiteral("shot");
    o.extension = QStringLiteral("png");
    o.padding = 3;
    o.numbering = ExportOptions::Numbering::OneBased;
    QCOMPARE(sequenceFileName(o, 0, 10, -1), QStringLiteral("shot.001.png"));
    o.numbering = ExportOptions::Numbering::ZeroBased;
    QCOMPARE(sequenceFileName(o, 0, 10, -1), QStringLiteral("shot.000.png"));
    o.numbering = ExportOptions::Numbering::MatchFrame;
    QCOMPARE(sequenceFileName(o, 0, 10, 1007), QStringLiteral("shot.1007.png"));  // wider than the padding
    QCOMPARE(sequenceFileName(o, 0, 10, -1), QStringLiteral("shot.011.png"));     // frame 10 is "11"
}

void FeaturesTest::exportValidation() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ExportOptions o;
    QVERIFY(!validateExport(o, QSize(32, 24)).isEmpty());  // no output path

    o.kind = ExportOptions::Kind::Video;
    o.outputPath = dir.filePath(QStringLiteral("out.mp4"));
    QVERIFY(validateExport(o, QSize(32, 24)).isEmpty());
    QVERIFY(!validateExport(o, QSize(33, 24)).isEmpty());  // odd width
    QFile file(o.outputPath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();
    QVERIFY(!validateExport(o, QSize(32, 24)).isEmpty());  // exists
    o.overwrite = true;
    QVERIFY(validateExport(o, QSize(32, 24)).isEmpty());

    o.kind = ExportOptions::Kind::ImageSequence;
    // o.outputPath still names the existing file, which is not a folder.
    QVERIFY(!validateExport(o, QSize(33, 25)).isEmpty());
    o.outputPath = dir.filePath(QStringLiteral("frames"));
    QVERIFY(validateExport(o, QSize(33, 25)).isEmpty());  // odd sizes are fine for images
    o.baseName.clear();
    QVERIFY(!validateExport(o, QSize(33, 25)).isEmpty());

    // The ffmpeg arguments read raw frames from stdin and name the output last.
    ExportOptions v;
    v.kind = ExportOptions::Kind::Video;
    v.outputPath = QStringLiteral("/tmp/o.mp4");
    v.videoCodec = QStringLiteral("libx264");
    const QStringList args = ffmpegArguments(v, QSize(64, 48), 25.0, QString(), 0.0, 0.0);
    QVERIFY(args.contains(QStringLiteral("rawvideo")));
    QVERIFY(args.contains(QStringLiteral("64x48")));
    QVERIFY(args.contains(QStringLiteral("libx264")));
    QCOMPARE(args.last(), QStringLiteral("/tmp/o.mp4"));
}

void FeaturesTest::exportsImageSequenceWithDrawings() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Session session;
    QString idA, idB;
    makeSeqs(session, dir.path(), &idA, &idB);
    QVERIFY(session.activateSource(idA));

    session.playback().seek(3);
    session.addStroke(lineStroke(QColor(0, 255, 0), 0.5, 6.0));
    session.playback().setRange(2, 4);

    ExportPlan plan;
    QString error;
    QVERIFY2(session.prepareExport(0, plan, &error), qPrintable(error));
    QCOMPARE(plan.frameCount, 10);
    QCOMPARE(plan.imageNumbers.size(), 10);
    QVERIFY(plan.strokes.contains(3));

    ExportOptions o;
    o.kind = ExportOptions::Kind::ImageSequence;
    o.frameSet = ExportOptions::FrameSet::Range;
    o.outputPath = dir.filePath(QStringLiteral("out"));
    o.baseName = QStringLiteral("x");
    o.extension = QStringLiteral("png");
    o.padding = 3;
    o.numbering = ExportOptions::Numbering::OneBased;

    std::atomic<bool> cancel{false};
    int lastDone = 0;
    const ExportResult result = runExport(plan, o, [&](int done, int) { lastDone = done; }, cancel);
    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(result.framesWritten, 3);
    QCOMPARE(lastDone, 3);

    const QDir out(o.outputPath);
    QCOMPARE(out.entryList({QStringLiteral("x.*.png")}).size(), 3);
    const QImage plain(out.filePath(QStringLiteral("x.001.png")));  // frame 2: no drawing
    const QImage drawn(out.filePath(QStringLiteral("x.002.png")));  // frame 3: the green line
    QVERIFY(!plain.isNull());
    QVERIFY(!drawn.isNull());
    QCOMPARE(plain.size(), QSize(kW, kH));
    QCOMPARE(qGreen(plain.pixel(kW / 2, kH / 2)), 0);
    QVERIFY(qGreen(drawn.pixel(kW / 2, kH / 2)) > 150);

    // Without annotations the same frame is plain; existing files block a second run.
    o.includeAnnotations = false;
    o.overwrite = true;
    const ExportResult again = runExport(plan, o, nullptr, cancel);
    QVERIFY2(again.ok, qPrintable(again.error));
    QCOMPARE(qGreen(QImage(out.filePath(QStringLiteral("x.002.png"))).pixel(kW / 2, kH / 2)), 0);
}

void FeaturesTest::exportCanBeCancelled() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Session session;
    QString idA, idB;
    makeSeqs(session, dir.path(), &idA, &idB);
    QVERIFY(session.activateSource(idA));

    ExportPlan plan;
    QString error;
    QVERIFY2(session.prepareExport(0, plan, &error), qPrintable(error));
    ExportOptions o;
    o.kind = ExportOptions::Kind::ImageSequence;
    o.outputPath = dir.filePath(QStringLiteral("never"));
    o.extension = QStringLiteral("png");

    std::atomic<bool> cancel{true};
    const ExportResult result = runExport(plan, o, nullptr, cancel);
    QVERIFY(!result.ok);
    QVERIFY(result.cancelled);

    // The threaded job reports the same through its signals.
    ExportPlan plan2;
    QVERIFY2(session.prepareExport(0, plan2, &error), qPrintable(error));
    o.outputPath = dir.filePath(QStringLiteral("job"));
    ExportJob job(std::move(plan2), o);
    QSignalSpy finished(&job, &ExportJob::finished);
    job.start();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
    QVERIFY(finished.first().at(0).toBool());
    QCOMPARE(finished.first().at(3).toInt(), 10);
}

void FeaturesTest::exportsVideoWithFfmpeg() {
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) QSKIP("ffmpeg is not installed");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Session session;
    QString idA, idB;
    makeSeqs(session, dir.path(), &idA, &idB);
    QVERIFY(session.activateSource(idA));

    ExportPlan plan;
    QString error;
    QVERIFY2(session.prepareExport(0, plan, &error), qPrintable(error));
    plan.fps = 10.0;

    ExportOptions o;
    o.kind = ExportOptions::Kind::Video;
    o.outputPath = dir.filePath(QStringLiteral("out.mp4"));
    o.extension = QStringLiteral("mp4");
    o.videoCodec = QStringLiteral("mpeg4");  // always built into ffmpeg, unlike libx264
    o.ffmpegPath = ffmpeg;

    std::atomic<bool> cancel{false};
    const ExportResult result = runExport(plan, o, nullptr, cancel);
    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(result.framesWritten, 10);
    QVERIFY(QFileInfo(o.outputPath).size() > 0);
#ifdef FR_HAVE_FFMPEG
    QString openError;
    const auto video = openFrameSource(o.outputPath, &openError);
    QVERIFY2(video != nullptr, qPrintable(openError));
    QCOMPARE(video->frameCount(), 10);
    QCOMPARE(video->size(), QSize(kW, kH));
#endif

    // A missing ffmpeg is reported, not crashed on.
    o.outputPath = dir.filePath(QStringLiteral("again.mp4"));
    o.ffmpegPath = dir.filePath(QStringLiteral("no-such-ffmpeg"));
    ExportPlan plan2;
    QVERIFY2(session.prepareExport(0, plan2, &error), qPrintable(error));
    const ExportResult failed = runExport(plan2, o, nullptr, cancel);
    QVERIFY(!failed.ok);
    QVERIFY(!failed.error.isEmpty());
}

// --- audio and preferences -----------------------------------------------------------------

void FeaturesTest::audioTrackDecodesSine() {
#ifdef FR_HAVE_FFMPEG
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) QSKIP("ffmpeg is not installed");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString wav = dir.filePath(QStringLiteral("tone.wav"));
    QProcess make;
    make.start(ffmpeg, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-f"), QStringLiteral("lavfi"),
                        QStringLiteral("-i"), QStringLiteral("sine=frequency=440:duration=1:sample_rate=44100"),
                        QStringLiteral("-y"), wav});
    QVERIFY(make.waitForFinished(30000));
    QCOMPARE(make.exitCode(), 0);

    AudioTrack track;
    QString error;
    QVERIFY2(track.open(wav, &error), qPrintable(error));
    QVERIFY(track.hasAudio());
    QTRY_VERIFY_WITH_TIMEOUT(track.isFinished(), 10000);
    // One second, resampled to 48 kHz; the resampler may add or drop a few frames.
    QVERIFY(qAbs(track.decodedFrames() - AudioTrack::kSampleRate) < 2000);

    const QByteArray block = track.read(1000, 480);
    QCOMPARE(block.size(), 480 * AudioTrack::kBytesPerFrame);
    const auto* samples = reinterpret_cast<const qint16*>(block.constData());
    int peak = 0;
    for (int i = 0; i < 480 * AudioTrack::kChannels; ++i) peak = qMax(peak, qAbs(static_cast<int>(samples[i])));
    QVERIFY(peak > 1000);

    // Before the start and after the end the track reads as silence, at the size asked for.
    const QByteArray before = track.read(-100, 100);
    QCOMPARE(before.size(), 100 * AudioTrack::kBytesPerFrame);
    QCOMPARE(before.count('\0'), before.size());
    const QByteArray after = track.read(track.decodedFrames() + 1000, 50);
    QCOMPARE(after.size(), 50 * AudioTrack::kBytesPerFrame);
    QCOMPARE(after.count('\0'), after.size());

    track.close();
    QVERIFY(!track.hasAudio());

    // A file without sound is refused with a reason.
    AudioTrack silent;
    QImage image(kW, kH, QImage::Format_RGB32);
    image.fill(Qt::black);
    const QString png = dir.filePath(QStringLiteral("x.png"));
    QVERIFY(image.save(png));
    QVERIFY(!silent.open(png, &error));
    QVERIFY(!error.isEmpty());
#else
    AudioTrack track;
    QString error;
    QVERIFY(!track.open(QStringLiteral("whatever.wav"), &error));
    QVERIFY(!error.isEmpty());
#endif
}

#ifdef FR_HAVE_FFMPEG
namespace {
// Runs ffmpeg with `args`; false when it fails.
bool runFfmpeg(const QString& ffmpeg, const QStringList& args) {
    QProcess make;
    make.start(ffmpeg, QStringList{QStringLiteral("-v"), QStringLiteral("error")} + args + QStringList{QStringLiteral("-y")});
    return make.waitForFinished(60000) && make.exitStatus() == QProcess::NormalExit && make.exitCode() == 0;
}
}  // namespace
#endif

void FeaturesTest::audioMixPlacesSegments() {
#ifdef FR_HAVE_FFMPEG
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) QSKIP("ffmpeg is not installed");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString lowWav = dir.filePath(QStringLiteral("low.wav"));
    const QString highWav = dir.filePath(QStringLiteral("high.wav"));
    QVERIFY(runFfmpeg(ffmpeg, {QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
                               QStringLiteral("sine=frequency=220:duration=1:sample_rate=48000"), lowWav}));
    QVERIFY(runFfmpeg(ffmpeg, {QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
                               QStringLiteral("sine=frequency=880:duration=1:sample_rate=48000"), highWav}));
    auto low = std::make_shared<AudioTrack>();
    auto high = std::make_shared<AudioTrack>();
    QVERIFY(low->open(lowWav) && high->open(highWav));
    QTRY_VERIFY_WITH_TIMEOUT(low->isFinished() && high->isFinished(), 10000);

    // low from its frame 2000 at 1000..1500, high from its frame 100 at 1500..2500, nothing after.
    AudioMix mix;
    QVERIFY(!mix.hasAudio());
    mix.setSegments({AudioMix::Segment{low, 1000, 500, 2000}, AudioMix::Segment{high, 1500, 1000, 100}});
    QVERIFY(mix.hasAudio());
    const int bpf = AudioTrack::kBytesPerFrame;
    const QByteArray all = mix.read(0, 3000);
    QCOMPARE(all.size(), 3000 * bpf);
    const QByteArray before = all.left(1000 * bpf);
    QCOMPARE(before.count('\0'), before.size());
    QCOMPARE(all.mid(1000 * bpf, 500 * bpf), low->read(2000, 500));
    QCOMPARE(all.mid(1500 * bpf, 1000 * bpf), high->read(100, 1000));
    const QByteArray after = all.mid(2500 * bpf);
    QCOMPARE(after.count('\0'), after.size());
    // A read that starts inside a segment and crosses into the next one.
    QCOMPARE(mix.read(1400, 200), low->read(2400, 100) + high->read(100, 100));

    // A segment without end (a plain source) and a negative track start (a delay) read silence first.
    mix.setSegments({AudioMix::Segment{low, 0, -1, -480}});
    QCOMPARE(mix.read(0, 960), QByteArray(480 * bpf, '\0') + low->read(0, 480));
#else
    QSKIP("this build has no FFmpeg");
#endif
}

// The reported bug: a timeline played without sound even when its clips had sound.
void FeaturesTest::timelineHasSound() {
#ifdef FR_HAVE_FFMPEG
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) QSKIP("ffmpeg is not installed");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString clip = dir.filePath(QStringLiteral("clip.mp4"));
    QVERIFY(runFfmpeg(ffmpeg, {QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
                               QStringLiteral("testsrc=size=64x48:rate=24:duration=1"), QStringLiteral("-f"),
                               QStringLiteral("lavfi"), QStringLiteral("-i"),
                               QStringLiteral("sine=frequency=440:duration=1:sample_rate=48000"),
                               QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"), QStringLiteral("-shortest"), clip}));

    Session session;
    AudioController audio(session);
    const QString video = session.importFile(clip);
    QVERIFY(!video.isEmpty());
    const QString images = session.importSequence(writeSequence(dir.filePath(QStringLiteral("seq")), QStringLiteral("s."), {1, 2, 3}));
    QVERIFY(!images.isEmpty());

    const QString silentTimeline = session.addTimeline(QStringLiteral("images"));
    session.addClipToTimeline(silentTimeline, images);
    QVERIFY(session.activateTimeline(silentTimeline));
    QVERIFY(!audio.hasTrack());  // image clips have no sound

    const QString timeline = session.addTimeline(QStringLiteral("mixed"));
    session.addClipToTimeline(timeline, images);
    session.addClipToTimeline(timeline, video);
    QVERIFY(session.activateTimeline(timeline));
    QCOMPARE(session.viewerClips().size(), 2);
    QVERIFY(audio.hasTrack());

    // Removing the clip with sound takes the sound away.
    session.removeClipFromTimeline(timeline, 1);
    QVERIFY(!audio.hasTrack());
#else
    QSKIP("this build has no FFmpeg");
#endif
}

void FeaturesTest::preferencesRoundTrip() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString ini = dir.filePath(QStringLiteral("prefs.ini"));
    {
        Preferences p(ini);
        QCOMPARE(p.apiPort(), quint16(4545));
        QVERIFY(!p.apiEnabled());
        QVERIFY(p.ffmpegPath().isEmpty());
        QVERIFY(p.recentFiles().isEmpty());

        p.setFfmpegPath(QStringLiteral("/opt/ffmpeg"));
        p.setCacheMegabytes(512);
        p.setAudioBufferMs(100);
        p.setSequenceFrameRate(25.0);
        p.setConfirmOnClose(false);
        p.setApi(true, 5000);
        p.setAudioMuted(true);
        p.setAudioVolume(0.4);
        p.setAudioScrub(false);
        p.setNumberingMode(3);
        p.setScrubPixelsPerFrame(4.0);
        p.setAlwaysOnTop(true);
        p.setShortcuts({{QStringLiteral("action.play"), QStringLiteral("P")},
                        {QStringLiteral("action.mute"), QString()}});
        for (int i = 0; i < 12; ++i) p.addRecentFile(QStringLiteral("/p/%1.kpro").arg(i));
        p.addRecentFile(QStringLiteral("/p/5.kpro"));  // moves to the front instead of repeating
        p.sync();
    }
    Preferences p(ini);
    QCOMPARE(p.ffmpegPath(), QStringLiteral("/opt/ffmpeg"));
    QCOMPARE(p.cacheMegabytes(), 512);
    QCOMPARE(p.audioBufferMs(), 100);
    QCOMPARE(p.sequenceFrameRate(), 25.0);
    QVERIFY(!p.confirmOnClose());
    QVERIFY(p.apiEnabled());
    QCOMPARE(p.apiPort(), quint16(5000));
    QVERIFY(p.audioMuted());
    QVERIFY(qAbs(p.audioVolume() - 0.4) < 1e-9);
    QVERIFY(!p.audioScrub());
    QCOMPARE(p.numberingMode(), 3);
    QCOMPARE(p.scrubPixelsPerFrame(), 4.0);
    QVERIFY(p.alwaysOnTop());
    const QHash<QString, QString> keys = p.shortcuts();
    QCOMPARE(keys.size(), 2);
    QCOMPARE(keys.value(QStringLiteral("action.play")), QStringLiteral("P"));
    QVERIFY(keys.contains(QStringLiteral("action.mute")) && keys.value(QStringLiteral("action.mute")).isEmpty());

    const QStringList recent = p.recentFiles();
    QCOMPARE(recent.size(), 10);
    QCOMPARE(recent.first(), QStringLiteral("/p/5.kpro"));
    QCOMPARE(recent.count(QStringLiteral("/p/5.kpro")), 1);
    QVERIFY(!recent.contains(QStringLiteral("/p/0.kpro")));  // pushed out by newer ones

    // Out-of-range values are held to the limits.
    p.setCacheMegabytes(1);
    QVERIFY(p.cacheMegabytes() >= 64);
    p.setAudioVolume(7.0);
    QVERIFY(p.audioVolume() <= 1.0);
    p.clearRecentFiles();
    QVERIFY(p.recentFiles().isEmpty());
}

QTEST_MAIN(FeaturesTest)
#include "test_features.moc"
