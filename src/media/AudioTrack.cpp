#include "media/AudioTrack.h"

#include <algorithm>
#include <cstring>
#include <utility>

#ifdef FR_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}
#endif

namespace fr {

AudioTrack::~AudioTrack() {
    close();
}

void AudioTrack::close() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    stop_ = false;
    std::lock_guard<std::mutex> lock(mutex_);
    pcm_.clear();
    pcm_.squeeze();
    opened_ = false;
    finished_ = false;
}

bool AudioTrack::open(const QString& path, QString* error) {
    close();
#ifdef FR_HAVE_FFMPEG
    std::promise<QString> probe;
    std::future<QString> outcome = probe.get_future();
    thread_ = std::thread([this, path, probe = std::move(probe)]() mutable { decode(path, probe); });
    const QString problem = outcome.get();  // empty: there is sound and decoding has started
    if (!problem.isEmpty()) {
        thread_.join();
        if (error) *error = problem;
        return false;
    }
    opened_ = true;
    return true;
#else
    Q_UNUSED(path);
    if (error) *error = QStringLiteral("This build has no FFmpeg, so there is no sound.");
    return false;
#endif
}

qint64 AudioTrack::decodedFrames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pcm_.size() / kBytesPerFrame;
}

QByteArray AudioTrack::read(qint64 startFrame, qint64 frames) const {
    if (frames <= 0) return {};
    QByteArray out(static_cast<qsizetype>(frames * kBytesPerFrame), '\0');
    std::lock_guard<std::mutex> lock(mutex_);
    const qint64 available = pcm_.size() / kBytesPerFrame;
    const qint64 from = std::max<qint64>(0, startFrame);
    const qint64 skip = from - startFrame;  // frames before the start of the track stay silent
    if (from >= available) return out;
    const qint64 count = std::min(frames - skip, available - from);
    if (count <= 0) return out;
    std::memcpy(out.data() + skip * kBytesPerFrame, pcm_.constData() + from * kBytesPerFrame,
                static_cast<std::size_t>(count * kBytesPerFrame));
    return out;
}

#ifdef FR_HAVE_FFMPEG
void AudioTrack::decode(const QString& path, std::promise<QString>& probe) {
    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    SwrContext* resampler = nullptr;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    const auto cleanup = [&] {
        swr_free(&resampler);
        avcodec_free_context(&codec);
        avformat_close_input(&format);
        av_packet_free(&packet);
        av_frame_free(&frame);
    };
    const auto fail = [&](const QString& why) {
        probe.set_value(why);
        cleanup();
    };

    const QByteArray utf8 = path.toUtf8();
    if (avformat_open_input(&format, utf8.constData(), nullptr, nullptr) < 0) {
        return fail(QStringLiteral("Cannot open %1").arg(path));
    }
    avformat_find_stream_info(format, nullptr);
    const AVCodec* decoder = nullptr;
    const int stream = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
    if (stream < 0 || !decoder) return fail(QStringLiteral("The file has no sound."));

    codec = avcodec_alloc_context3(decoder);
    if (!codec || avcodec_parameters_to_context(codec, format->streams[stream]->codecpar) < 0 ||
        avcodec_open2(codec, decoder, nullptr) < 0) {
        return fail(QStringLiteral("Cannot open the audio decoder."));
    }

    AVChannelLayout inputLayout{};
    AVChannelLayout outputLayout{};
    av_channel_layout_default(&outputLayout, kChannels);
    if (codec->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
        av_channel_layout_default(&inputLayout, codec->ch_layout.nb_channels > 0 ? codec->ch_layout.nb_channels : kChannels);
    } else {
        av_channel_layout_copy(&inputLayout, &codec->ch_layout);
    }
    const int status = swr_alloc_set_opts2(&resampler, &outputLayout, AV_SAMPLE_FMT_S16, kSampleRate,
                                           &inputLayout, codec->sample_fmt, codec->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&inputLayout);
    av_channel_layout_uninit(&outputLayout);
    if (status < 0 || swr_init(resampler) < 0) return fail(QStringLiteral("Cannot set up the audio resampler."));
    probe.set_value(QString());

    // Converts one decoded frame (or, with nullptr, what the resampler still holds) and appends it.
    const auto append = [&](const AVFrame* decoded) {
        const int incoming = decoded ? decoded->nb_samples : 0;
        const int capacity = static_cast<int>(av_rescale_rnd(swr_get_delay(resampler, codec->sample_rate) + incoming,
                                                             kSampleRate, codec->sample_rate, AV_ROUND_UP)) + 32;
        QByteArray buffer(capacity * kBytesPerFrame, Qt::Uninitialized);
        uint8_t* output[1] = {reinterpret_cast<uint8_t*>(buffer.data())};
        const int produced = swr_convert(resampler, output, capacity,
                                         decoded ? decoded->extended_data : nullptr, incoming);
        if (produced <= 0) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (pcm_.size() < kMaxBytes) pcm_.append(buffer.constData(), static_cast<qsizetype>(produced) * kBytesPerFrame);
    };

    while (!stop_ && av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == stream && avcodec_send_packet(codec, packet) >= 0) {
            while (avcodec_receive_frame(codec, frame) >= 0) {
                append(frame);
                av_frame_unref(frame);
            }
        }
        av_packet_unref(packet);
    }
    if (!stop_) {
        avcodec_send_packet(codec, nullptr);
        while (avcodec_receive_frame(codec, frame) >= 0) {
            append(frame);
            av_frame_unref(frame);
        }
        append(nullptr);
    }
    finished_ = true;
    cleanup();
}
#endif

}  // namespace fr
