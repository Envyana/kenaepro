#pragma once

#include <QImage>
#include <QSize>
#include <QString>
#include <QVector>

#include <memory>

namespace fr {

// Random access to the frames of one media file. Frame indices are 0-based.
// Implementations are not thread safe: one owner decodes at a time.
class FrameSource {
public:
    virtual ~FrameSource() = default;

    virtual bool open(const QString& path, QString* error) = 0;
    virtual int frameCount() const = 0;
    virtual double fps() const = 0;  // 0 when the source has no intrinsic rate (still images)
    virtual QSize size() const = 0;
    // The decoded frame, or a null image when it cannot be produced.
    virtual QImage frame(int index) = 0;
    // For image sequences: the number in each frame's file name. Empty for everything else.
    virtual QVector<int> frameNumbers() const { return {}; }
    // True when frame() may run on several threads at once (each frame is its own file). A video
    // decoder reads one stream in order, so it says no.
    virtual bool concurrentFrames() const { return false; }
};

// A single still image, shown as a one-frame clip. Works without FFmpeg.
class StillImageSource final : public FrameSource {
public:
    bool open(const QString& path, QString* error) override;
    int frameCount() const override { return image_.isNull() ? 0 : 1; }
    double fps() const override { return 0.0; }
    QSize size() const override { return image_.size(); }
    QImage frame(int index) override;
    bool concurrentFrames() const override { return true; }

private:
    QImage image_;
};

// Picks an implementation for `path`: still image first, then video (when built with FFmpeg).
std::unique_ptr<FrameSource> openFrameSource(const QString& path, QString* error = nullptr);

// True when this build can decode video.
bool videoDecodingAvailable();

}  // namespace fr
