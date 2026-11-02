#pragma once

#include "core/ProjectModel.h"
#include "media/FrameSource.h"

#include <QHash>
#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

namespace fr {

// Everything an export needs, copied out of the session so it can run on its own thread: its own
// decoder (not the one the viewer uses) and the drawings to burn in. Frame numbers are 0-based
// positions in what the viewer shows.
struct ExportPlan {
    std::unique_ptr<FrameSource> frames;
    double fps = 24.0;
    int frameCount = 0;
    int rangeStart = 0;                   // the playback range, inclusive
    int rangeEnd = -1;                    // -1: the last frame
    QHash<int, QVector<Stroke>> strokes;  // by frame, with "hold" already applied
    QVector<int> imageNumbers;            // file numbers of an image sequence, else empty
    QVector<int> bookmarkedFrames;        // frames with a frame bookmark, ascending
    QString audioPath;                    // only when everything shown is one video with sound
    double audioDelayMs = 0.0;
};

struct ExportOptions {
    enum class Kind { Video, ImageSequence };
    enum class FrameSet { All, Range, Bookmarks };
    enum class Numbering { ZeroBased, OneBased, MatchFrame };

    Kind kind = Kind::Video;
    FrameSet frameSet = FrameSet::All;
    Numbering numbering = Numbering::OneBased;
    QString outputPath;               // the video file, or the folder of an image sequence
    QString baseName = QStringLiteral("frame");  // image sequences: base.0001.png
    QString extension = QStringLiteral("mp4");   // the container, or the image format
    QString videoCodec = QStringLiteral("libx264");  // libx264, prores_ks, libvpx-vp9 or mpeg4
    int padding = 4;
    int width = 0;   // 0: the size of the first frame
    int height = 0;
    bool includeAnnotations = true;
    bool includeAudio = true;
    bool overwrite = false;
    QString ffmpegPath;               // empty: look in PATH
};

struct ExportResult {
    bool ok = false;
    bool cancelled = false;
    QString error;
    int framesWritten = 0;
};

// --- pieces that can be tested on their own -------------------------------------------------

// The frames to write, in order. Bookmarks must be ascending.
QVector<int> framesToExport(int frameCount, int rangeStart, int rangeEnd, const QVector<int>& bookmarked,
                            ExportOptions::FrameSet set);
// "base.0007.png". `ordinal` counts exported frames from 0, `frame` is the position in what is
// shown, `imageNumber` the number in the source file's name or -1.
QString sequenceFileName(const ExportOptions& options, int ordinal, int frame, int imageNumber);
// What is wrong with the options for a picture of `size`, or an empty string.
QString validateExport(const ExportOptions& options, QSize size);
// The command line for ffmpeg reading raw BGRA frames from stdin. `audioStartSeconds` may be
// negative, which delays the sound instead of skipping into it.
QStringList ffmpegArguments(const ExportOptions& options, QSize size, double fps, const QString& audioPath,
                            double audioStartSeconds, double audioSeconds);
// One output frame: the picture fitted into `size` on black, with the strokes on top. Format
// ARGB32, which is BGRA in memory on little-endian machines.
QImage renderExportFrame(const QImage& source, const QVector<Stroke>& strokes, QSize size, bool burnIn);

using ExportProgress = std::function<void(int done, int total)>;
// Runs the whole export on the calling thread.
ExportResult runExport(ExportPlan& plan, const ExportOptions& options, const ExportProgress& progress,
                       const std::atomic<bool>& cancel);

// Runs an export on its own thread. Signals arrive on the thread that owns the job.
class ExportJob : public QObject {
    Q_OBJECT
public:
    ExportJob(ExportPlan plan, ExportOptions options, QObject* parent = nullptr);
    ~ExportJob() override;  // cancels and waits for the thread

    void start();
    void cancel() { cancel_ = true; }

signals:
    void progress(int done, int total);
    void finished(bool ok, bool cancelled, const QString& error, int framesWritten);

private:
    ExportPlan plan_;
    ExportOptions options_;
    std::atomic<bool> cancel_{false};
    std::thread thread_;
};

}  // namespace fr
