#include "media/FfmpegFrameSource.h"

#include <algorithm>
#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
}

namespace fr {
namespace {

QString avErrorText(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof buffer);
    return QString::fromUtf8(buffer);
}

bool fail(QString* error, const QString& message) {
    if (error) *error = message;
    return false;
}

}  // namespace

FfmpegFrameSource::FfmpegFrameSource() = default;

FfmpegFrameSource::~FfmpegFrameSource() {
    close();
}

void FfmpegFrameSource::close() {
    sws_freeContext(sws_);
    sws_ = nullptr;
    av_frame_free(&frame_);
    av_packet_free(&packet_);
    avcodec_free_context(&codec_);
    avformat_close_input(&format_);
    stream_ = nullptr;
    videoIndex_ = -1;
    frameCount_ = 0;
    lastIndex_ = -1;
    lastImage_ = QImage();
}

bool FfmpegFrameSource::open(const QString& path, QString* error) {
    close();

    const QByteArray utf8 = path.toUtf8();
    int rc = avformat_open_input(&format_, utf8.constData(), nullptr, nullptr);
    if (rc < 0) return fail(error, QStringLiteral("Cannot open %1: %2").arg(path, avErrorText(rc)));

    rc = avformat_find_stream_info(format_, nullptr);
    if (rc < 0) {
        close();
        return fail(error, QStringLiteral("Cannot read stream info: %1").arg(avErrorText(rc)));
    }

    const AVCodec* decoder = nullptr;
    videoIndex_ = av_find_best_stream(format_, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (videoIndex_ < 0 || !decoder) {
        close();
        return fail(error, QStringLiteral("%1 has no decodable video stream").arg(path));
    }
    stream_ = format_->streams[videoIndex_];

    codec_ = avcodec_alloc_context3(decoder);
    if (!codec_ || avcodec_parameters_to_context(codec_, stream_->codecpar) < 0) {
        close();
        return fail(error, QStringLiteral("Cannot set up the video decoder"));
    }
    codec_->thread_count = 0;  // let FFmpeg choose
    codec_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    rc = avcodec_open2(codec_, decoder, nullptr);
    if (rc < 0) {
        close();
        return fail(error, QStringLiteral("Cannot open the video decoder: %1").arg(avErrorText(rc)));
    }

    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    if (!frame_ || !packet_) {
        close();
        return fail(error, QStringLiteral("Out of memory"));
    }

    AVRational rate = av_guess_frame_rate(format_, stream_, nullptr);
    if (rate.num <= 0 || rate.den <= 0) rate = AVRational{24, 1};
    rateNum_ = rate.num;
    rateDen_ = rate.den;
    fps_ = av_q2d(rate);

    startTs_ = stream_->start_time != AV_NOPTS_VALUE ? stream_->start_time : 0;
    frameDurationTs_ = std::max<long long>(1, av_rescale_q(1, av_inv_q(rate), stream_->time_base));
    size_ = QSize(codec_->width, codec_->height);

    if (stream_->nb_frames > 0) {
        frameCount_ = static_cast<int>(stream_->nb_frames);
    } else {
        double seconds = 0.0;
        if (stream_->duration != AV_NOPTS_VALUE) {
            seconds = static_cast<double>(stream_->duration) * av_q2d(stream_->time_base);
        } else if (format_->duration != AV_NOPTS_VALUE) {
            seconds = static_cast<double>(format_->duration) / AV_TIME_BASE;
        }
        frameCount_ = static_cast<int>(std::llround(seconds * fps_));
    }
    frameCount_ = std::max(1, frameCount_);
    return true;
}

QImage FfmpegFrameSource::frame(int index) {
    if (!format_) return {};
    index = std::clamp(index, 0, frameCount_ - 1);
    if (index == lastIndex_ && !lastImage_.isNull()) return lastImage_;

    const AVRational rate{rateNum_, rateDen_};
    const long long target = startTs_ + av_rescale_q(index, av_inv_q(rate), stream_->time_base);

    // Playing forward decodes the next frame directly; any other jump seeks to the previous
    // keyframe and decodes forward from there.
    const bool sequential = lastIndex_ >= 0 && index == lastIndex_ + 1;
    if (!sequential) {
        av_seek_frame(format_, videoIndex_, target, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(codec_);
    }
    // A frame that cannot be decoded (a damaged file, or an estimated frame count that runs past
    // the real end) is reported as null, never as the previous picture under the wrong index.
    if (!decodeToFrame(target)) return {};

    const QImage image = convertFrame(frame_);
    av_frame_unref(frame_);
    if (image.isNull()) return {};
    lastImage_ = image;
    lastIndex_ = index;
    return lastImage_;
}

// Decodes until a frame at or after `targetTs` (within half a frame) is in frame_.
bool FfmpegFrameSource::decodeToFrame(long long targetTs) {
    const long long threshold = targetTs - frameDurationTs_ / 2;
    bool draining = false;
    for (;;) {
        int rc = avcodec_receive_frame(codec_, frame_);
        if (rc == 0) {
            const long long pts = frame_->best_effort_timestamp;
            if (pts == AV_NOPTS_VALUE || pts >= threshold) return true;
            av_frame_unref(frame_);
            continue;
        }
        if (rc != AVERROR(EAGAIN) || draining) return false;

        rc = av_read_frame(format_, packet_);
        if (rc < 0) {
            avcodec_send_packet(codec_, nullptr);  // end of file: flush the decoder
            draining = true;
            continue;
        }
        if (packet_->stream_index == videoIndex_) avcodec_send_packet(codec_, packet_);
        av_packet_unref(packet_);
    }
}

QImage FfmpegFrameSource::convertFrame(const AVFrame* source) {
    // TODO: the matrix and range are left to swscale's defaults (BT.601, limited). Read them
    // from the frame (colorspace, color_range) and call sws_setColorspaceDetails.
    sws_ = sws_getCachedContext(sws_, source->width, source->height,
                                static_cast<AVPixelFormat>(source->format), source->width,
                                source->height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr,
                                nullptr);
    if (!sws_) return {};

    // BGRA bytes are what QImage::Format_RGB32 holds on little-endian machines.
    QImage image(source->width, source->height, QImage::Format_RGB32);
    if (image.isNull()) return {};
    uint8_t* dst[4] = {image.bits(), nullptr, nullptr, nullptr};
    const int dstStride[4] = {static_cast<int>(image.bytesPerLine()), 0, 0, 0};
    sws_scale(sws_, source->data, source->linesize, 0, source->height, dst, dstStride);
    return image;
}

}  // namespace fr
