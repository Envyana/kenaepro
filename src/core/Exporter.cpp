#include "core/Exporter.h"

#include "core/StrokeRenderer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageWriter>
#include <QMetaObject>
#include <QPainter>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryFile>

#include <algorithm>
#include <cmath>

namespace fr {
namespace {

QString findFfmpeg(const ExportOptions& options) {
    if (!options.ffmpegPath.isEmpty()) return options.ffmpegPath;
    return QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
}

QStringList codecArguments(const QString& codec) {
    if (codec == QLatin1String("prores_ks")) {
        return {QStringLiteral("-c:v"), QStringLiteral("prores_ks"), QStringLiteral("-profile:v"),
                QStringLiteral("3"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv422p10le")};
    }
    if (codec == QLatin1String("libvpx-vp9")) {
        return {QStringLiteral("-c:v"), QStringLiteral("libvpx-vp9"), QStringLiteral("-crf"),
                QStringLiteral("28"), QStringLiteral("-b:v"), QStringLiteral("0"),
                QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p")};
    }
    if (codec == QLatin1String("mpeg4")) {
        return {QStringLiteral("-c:v"), QStringLiteral("mpeg4"), QStringLiteral("-q:v"),
                QStringLiteral("2"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p")};
    }
    return {QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-crf"), QStringLiteral("18"),
            QStringLiteral("-preset"), QStringLiteral("medium"), QStringLiteral("-pix_fmt"),
            QStringLiteral("yuv420p")};
}

QString logTail(const QString& logPath) {
    QFile file(logPath);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(file.readAll().right(800)).trimmed();
}

ExportResult runVideo(ExportPlan& plan, const ExportOptions& options, const QVector<int>& frames, QSize size,
                      const QImage& firstImage, const ExportProgress& progress, const std::atomic<bool>& cancel) {
    ExportResult result;
    const QString ffmpeg = findFfmpeg(options);
    if (ffmpeg.isEmpty()) {
        result.error = QStringLiteral("ffmpeg was not found. Install it or set its path in Preferences.");
        return result;
    }
    QDir().mkpath(QFileInfo(options.outputPath).absolutePath());

    // The sound is cut from one stretch of the track, so it only fits frames that follow each other.
    const bool contiguous = options.frameSet != ExportOptions::FrameSet::Bookmarks;
    const QString audioPath = contiguous ? plan.audioPath : QString();
    const double audioStart = frames.first() / plan.fps - plan.audioDelayMs / 1000.0;
    const double audioSeconds = static_cast<double>(frames.size()) / plan.fps;

    QTemporaryFile log;  // ffmpeg's messages go to a file so a full pipe can never stall it
    if (!log.open()) {
        result.error = QStringLiteral("Could not create a temporary file for the ffmpeg log.");
        return result;
    }
    QProcess process;
    process.setStandardErrorFile(log.fileName());
    process.setStandardOutputFile(QProcess::nullDevice());
    process.start(ffmpeg, ffmpegArguments(options, size, plan.fps, audioPath, audioStart, audioSeconds));
    if (!process.waitForStarted(10000)) {
        result.error = QStringLiteral("Could not start ffmpeg: %1").arg(process.errorString());
        return result;
    }

    const auto abort = [&](const QString& why) {
        process.kill();
        process.waitForFinished(2000);
        QFile::remove(options.outputPath);
        result.error = why;
        return result;
    };

    for (int ordinal = 0; ordinal < frames.size(); ++ordinal) {
        if (cancel) {
            process.kill();
            process.waitForFinished(2000);
            QFile::remove(options.outputPath);
            result.cancelled = true;
            return result;
        }
        const int frame = frames[ordinal];
        const QImage image = ordinal == 0 ? firstImage : plan.frames->frame(frame);
        if (image.isNull()) return abort(QStringLiteral("Frame %1 could not be decoded.").arg(frame + 1));

        const QImage out = renderExportFrame(image, plan.strokes.value(frame), size, options.includeAnnotations);
        if (process.write(reinterpret_cast<const char*>(out.constBits()), out.sizeInBytes()) < 0) {
            return abort(QStringLiteral("Writing to ffmpeg failed."));
        }
        while (process.bytesToWrite() > 8 * 1024 * 1024) {  // let ffmpeg catch up
            if (!process.waitForBytesWritten(2000) && process.state() != QProcess::Running) break;
        }
        if (process.state() != QProcess::Running) {
            return abort(QStringLiteral("ffmpeg stopped early: %1").arg(logTail(log.fileName())));
        }
        ++result.framesWritten;
        if (progress) progress(ordinal + 1, static_cast<int>(frames.size()));
    }

    process.closeWriteChannel();
    // Most of the encoding of a short clip happens here, so keep watching for a cancel.
    while (process.state() != QProcess::NotRunning) {
        if (cancel) {
            process.kill();
            process.waitForFinished(2000);
            QFile::remove(options.outputPath);
            result.cancelled = true;
            result.framesWritten = 0;
            return result;
        }
        process.waitForFinished(100);
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString why = QStringLiteral("ffmpeg failed: %1").arg(logTail(log.fileName()));
        QFile::remove(options.outputPath);
        result.framesWritten = 0;
        result.error = why;
        return result;
    }
    result.ok = true;
    return result;
}

ExportResult runSequence(ExportPlan& plan, const ExportOptions& options, const QVector<int>& frames, QSize size,
                         const QImage& firstImage, const ExportProgress& progress, const std::atomic<bool>& cancel) {
    ExportResult result;
    if (!QDir().mkpath(options.outputPath)) {
        result.error = QStringLiteral("Could not create the folder %1").arg(options.outputPath);
        return result;
    }
    const QDir directory(options.outputPath);
    const bool isJpeg = options.extension.compare(QLatin1String("jpg"), Qt::CaseInsensitive) == 0 ||
                        options.extension.compare(QLatin1String("jpeg"), Qt::CaseInsensitive) == 0;

    for (int ordinal = 0; ordinal < frames.size(); ++ordinal) {
        if (cancel) {
            result.cancelled = true;
            return result;
        }
        const int frame = frames[ordinal];
        const QImage image = ordinal == 0 ? firstImage : plan.frames->frame(frame);
        if (image.isNull()) {
            result.error = QStringLiteral("Frame %1 could not be decoded.").arg(frame + 1);
            return result;
        }
        const QString path = directory.filePath(
            sequenceFileName(options, ordinal, frame, plan.imageNumbers.value(frame, -1)));
        if (QFileInfo::exists(path) && !options.overwrite) {
            result.error = QStringLiteral("%1 already exists (enable overwrite to replace it).").arg(path);
            return result;
        }

        QImage out = renderExportFrame(image, plan.strokes.value(frame), size, options.includeAnnotations);
        if (isJpeg) out = out.convertToFormat(QImage::Format_RGB32);
        QImageWriter writer(path, options.extension.toLatin1());
        if (isJpeg) writer.setQuality(95);
        if (!writer.write(out)) {
            result.error = QStringLiteral("Could not write %1: %2").arg(path, writer.errorString());
            return result;
        }
        ++result.framesWritten;
        if (progress) progress(ordinal + 1, static_cast<int>(frames.size()));
    }
    result.ok = true;
    return result;
}

}  // namespace

QVector<int> framesToExport(int frameCount, int rangeStart, int rangeEnd, const QVector<int>& bookmarked,
                            ExportOptions::FrameSet set) {
    QVector<int> frames;
    if (frameCount <= 0) return frames;
    const int last = frameCount - 1;
    switch (set) {
        case ExportOptions::FrameSet::All:
            for (int i = 0; i <= last; ++i) frames.push_back(i);
            break;
        case ExportOptions::FrameSet::Range: {
            const int first = std::clamp(rangeStart, 0, last);
            const int end = rangeEnd < 0 ? last : std::clamp(rangeEnd, first, last);
            for (int i = first; i <= end; ++i) frames.push_back(i);
            break;
        }
        case ExportOptions::FrameSet::Bookmarks:
            for (const int frame : bookmarked) {
                if (frame >= 0 && frame <= last) frames.push_back(frame);
            }
            break;
    }
    return frames;
}

QString sequenceFileName(const ExportOptions& options, int ordinal, int frame, int imageNumber) {
    int number = 0;
    switch (options.numbering) {
        case ExportOptions::Numbering::ZeroBased: number = ordinal; break;
        case ExportOptions::Numbering::OneBased: number = ordinal + 1; break;
        case ExportOptions::Numbering::MatchFrame: number = imageNumber >= 0 ? imageNumber : frame + 1; break;
    }
    return options.baseName + QLatin1Char('.') +
           QString::number(number).rightJustified(std::max(1, options.padding), QLatin1Char('0')) +
           QLatin1Char('.') + options.extension;
}

QString validateExport(const ExportOptions& options, QSize size) {
    if (options.outputPath.isEmpty()) return QStringLiteral("The output path is not set.");
    if (options.kind == ExportOptions::Kind::Video) {
        if (QFileInfo::exists(options.outputPath) && !options.overwrite) {
            return QStringLiteral("The output file already exists (enable overwrite to replace it).");
        }
        if (size.width() % 2 != 0 || size.height() % 2 != 0) {
            return QStringLiteral("The resolution cannot contain an odd value (%1x%2).")
                .arg(size.width())
                .arg(size.height());
        }
    } else {
        const QFileInfo info(options.outputPath);
        if (info.exists() && !info.isDir()) return QStringLiteral("The output path is not a folder.");
        if (options.baseName.isEmpty()) return QStringLiteral("The file name is empty.");
    }
    return {};
}

QStringList ffmpegArguments(const ExportOptions& options, QSize size, double fps, const QString& audioPath,
                            double audioStartSeconds, double audioSeconds) {
    const auto number = [](double value) { return QString::number(value, 'f', 6); };
    QStringList args = {QStringLiteral("-y"), QStringLiteral("-v"), QStringLiteral("error"),
                        QStringLiteral("-f"), QStringLiteral("rawvideo"), QStringLiteral("-pix_fmt"),
                        QStringLiteral("bgra"), QStringLiteral("-s"),
                        QStringLiteral("%1x%2").arg(size.width()).arg(size.height()),
                        QStringLiteral("-r"), number(fps), QStringLiteral("-i"), QStringLiteral("-")};

    const bool withAudio = options.includeAudio && !audioPath.isEmpty();
    if (withAudio) {
        if (audioStartSeconds >= 0.0) {
            args << QStringLiteral("-ss") << number(audioStartSeconds);
        } else {
            args << QStringLiteral("-itsoffset") << number(-audioStartSeconds);
        }
        args << QStringLiteral("-t") << number(audioSeconds) << QStringLiteral("-i") << audioPath;
    }

    args << QStringLiteral("-map") << QStringLiteral("0:v");
    if (withAudio) args << QStringLiteral("-map") << QStringLiteral("1:a:0?");
    args += codecArguments(options.videoCodec);
    if (withAudio) {
        const bool webm = options.extension.compare(QLatin1String("webm"), Qt::CaseInsensitive) == 0;
        args << QStringLiteral("-c:a") << (webm ? QStringLiteral("libopus") : QStringLiteral("aac"))
             << QStringLiteral("-shortest");
    }
    const QString extension = options.extension.toLower();
    if (extension == QLatin1String("mp4") || extension == QLatin1String("mov")) {
        args << QStringLiteral("-movflags") << QStringLiteral("+faststart");
    }
    args << options.outputPath;
    return args;
}

QImage renderExportFrame(const QImage& source, const QVector<Stroke>& strokes, QSize size, bool burnIn) {
    QImage out(size, QImage::Format_ARGB32);
    out.fill(Qt::black);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QSizeF fitted = QSizeF(source.size()).scaled(QSizeF(size), Qt::KeepAspectRatio);
    const QRectF target((size.width() - fitted.width()) / 2.0, (size.height() - fitted.height()) / 2.0,
                        fitted.width(), fitted.height());
    painter.drawImage(target, source);
    if (burnIn) paintStrokes(painter, strokes, target, source.width());
    painter.end();
    return out;
}

ExportResult runExport(ExportPlan& plan, const ExportOptions& options, const ExportProgress& progress,
                       const std::atomic<bool>& cancel) {
    ExportResult result;
    if (!plan.frames || plan.frameCount <= 0) {
        result.error = QStringLiteral("There is nothing to export.");
        return result;
    }
    const QVector<int> frames =
        framesToExport(plan.frameCount, plan.rangeStart, plan.rangeEnd, plan.bookmarkedFrames, options.frameSet);
    if (frames.isEmpty()) {
        result.error = QStringLiteral("No frames match the chosen selection (are there any bookmarks?).");
        return result;
    }

    const QImage first = plan.frames->frame(frames.first());
    if (first.isNull()) {
        result.error = QStringLiteral("The first frame could not be decoded.");
        return result;
    }
    const QSize size(options.width > 0 ? options.width : first.width(),
                     options.height > 0 ? options.height : first.height());
    const QString problem = validateExport(options, size);
    if (!problem.isEmpty()) {
        result.error = problem;
        return result;
    }
    return options.kind == ExportOptions::Kind::Video
               ? runVideo(plan, options, frames, size, first, progress, cancel)
               : runSequence(plan, options, frames, size, first, progress, cancel);
}

ExportJob::ExportJob(ExportPlan plan, ExportOptions options, QObject* parent)
    : QObject(parent), plan_(std::move(plan)), options_(std::move(options)) {}

ExportJob::~ExportJob() {
    cancel_ = true;
    if (thread_.joinable()) thread_.join();
}

void ExportJob::start() {
    thread_ = std::thread([this] {
        const ExportResult result = runExport(
            plan_, options_,
            [this](int done, int total) {
                QMetaObject::invokeMethod(this, [this, done, total] { emit progress(done, total); },
                                          Qt::QueuedConnection);
            },
            cancel_);
        QMetaObject::invokeMethod(
            this,
            [this, result] { emit finished(result.ok, result.cancelled, result.error, result.framesWritten); },
            Qt::QueuedConnection);
    });
}

}  // namespace fr
