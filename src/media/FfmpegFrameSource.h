#pragma once

#include "media/FrameSource.h"

// FFmpeg types stay out of this header so the rest of the app does not need its include path.
struct AVFormatContext;
struct AVCodecContext;
struct AVStream;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace fr {

// Frame-accurate video decoding on top of libavformat/libavcodec (FFmpeg 6 or 7).
// Frame n is the frame whose timestamp is closest to n / fps; this assumes constant frame
// rate, which is the common case for review material. Variable frame rate is a known gap.
class FfmpegFrameSource final : public FrameSource {
public:
    FfmpegFrameSource();
    ~FfmpegFrameSource() override;
    FfmpegFrameSource(const FfmpegFrameSource&) = delete;
    FfmpegFrameSource& operator=(const FfmpegFrameSource&) = delete;

    bool open(const QString& path, QString* error) override;
    int frameCount() const override { return frameCount_; }
    double fps() const override { return fps_; }
    QSize size() const override { return size_; }
    QImage frame(int index) override;

private:
    void close();
    bool decodeToFrame(long long targetTs);
    QImage convertFrame(const AVFrame* source);

    AVFormatContext* format_ = nullptr;
    AVCodecContext* codec_ = nullptr;
    AVStream* stream_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    SwsContext* sws_ = nullptr;

    int videoIndex_ = -1;
    int rateNum_ = 24;
    int rateDen_ = 1;
    double fps_ = 0.0;
    long long startTs_ = 0;
    long long frameDurationTs_ = 1;
    int frameCount_ = 0;
    QSize size_;

    int lastIndex_ = -1;
    QImage lastImage_;
};

}  // namespace fr
