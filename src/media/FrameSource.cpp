#include "media/FrameSource.h"
#include "media/ImageSequence.h"

#include <QImageReader>

#ifdef FR_HAVE_FFMPEG
#include "media/FfmpegFrameSource.h"
#endif

namespace fr {

bool StillImageSource::open(const QString& path, QString* error) {
    QImageReader reader(path);
    reader.setAutoTransform(true);
    image_ = reader.read();
    if (image_.isNull()) {
        if (error) *error = reader.errorString();
        return false;
    }
    return true;
}

QImage StillImageSource::frame(int /*index*/) {
    return image_;
}

bool videoDecodingAvailable() {
#ifdef FR_HAVE_FFMPEG
    return true;
#else
    return false;
#endif
}

std::unique_ptr<FrameSource> openFrameSource(const QString& path, QString* error) {
    if (isSequencePath(path)) {
        auto sequence = std::make_unique<ImageSequenceSource>();
        if (sequence->open(path, error)) return sequence;
        return nullptr;
    }

    QString stillError;
    auto still = std::make_unique<StillImageSource>();
    if (still->open(path, &stillError)) return still;

#ifdef FR_HAVE_FFMPEG
    auto video = std::make_unique<FfmpegFrameSource>();
    QString videoError;
    if (video->open(path, &videoError)) return video;
    if (error) *error = videoError;
    return nullptr;
#else
    if (error) {
        *error = QStringLiteral("Cannot open %1 as an image (%2). Video needs a build with FFmpeg.")
                     .arg(path, stillError);
    }
    return nullptr;
#endif
}

}  // namespace fr
