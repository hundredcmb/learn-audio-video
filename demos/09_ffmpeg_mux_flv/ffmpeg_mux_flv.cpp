extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <fstream>

thread_local static char error_buffer[AV_ERROR_MAX_STRING_SIZE] = {};
static constexpr int kDurationSeconds = 10;
int video_count = 0, audio_count = 0;

static const AVPixelFormat kVideoPixelFormat = AV_PIX_FMT_YUV420P;
static constexpr int kVideoFrameRate = 25;
static constexpr int kVideoWidth = 640;
static constexpr int kVideoHeight = 360;
static constexpr int64_t kVideoBitrate = 1000000;

static const AVSampleFormat kAudioSampleFormat = AV_SAMPLE_FMT_FLTP;
static constexpr int kAudioProfile = FF_PROFILE_AAC_LOW;
static constexpr int kAudioChannels = 2;
static constexpr int kAudioSampleRate = 48000;
static constexpr int64_t kAudioBitrate = 128 * 1024;

static char *ErrorToString(const int error_code) {
    std::memset(error_buffer, 0, AV_ERROR_MAX_STRING_SIZE);
    return av_make_error_string(error_buffer, AV_ERROR_MAX_STRING_SIZE,
                                error_code);
}

struct OutputStream {
    AVStream *stream;
    AVCodecContext *codec_ctx;
    AVFrame *frame;
    AVPacket *packet;
    int64_t next_pts;
};

static int WriteVideoFrame(AVFormatContext *fmt_ctx, OutputStream *video_stream) {
    int compare_ts = av_compare_ts(video_stream->next_pts,
                                   video_stream->codec_ctx->time_base,
                                   kDurationSeconds,
                                   AVRational(1, 1));
    if (compare_ts >= 0) {
        return 1;
    }

    printf("\nwrite_video_frame\n");
    video_stream->next_pts += 1;
    return 0;
}

static int WriteAudioFrame(AVFormatContext *fmt_ctx, OutputStream *audio_stream) {
    int compare_ts = av_compare_ts(audio_stream->next_pts,
                                   audio_stream->codec_ctx->time_base,
                                   kDurationSeconds,
                                   AVRational(1, 1));
    if (compare_ts >= 0) {
        return 1;
    }

    printf("\nwrite_audio_frame\n");
    audio_stream->next_pts += 1024;
    return 0;
}

static int InnerMultiplexFLV(AVFormatContext *fmt_ctx,
                             OutputStream *audio_stream,
                             OutputStream *video_stream) {
    int error_code = 0;
    int encode_video = 1, encode_audio = 1;
    int compare_ts = 0;

    if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        if ((error_code = avio_open(&fmt_ctx->pb, fmt_ctx->url,
                                    AVIO_FLAG_WRITE)) < 0) {
            fprintf(stderr, "Failed to avio_open: %s\n",
                    ErrorToString(error_code));
            return error_code;
        }
    }

    if ((error_code = avformat_write_header(fmt_ctx, nullptr) < 0)) {
        fprintf(stderr, "Failed to avformat_write_header: %s\n",
                ErrorToString(error_code));
        goto end_mux_flv;
    }

    while (encode_audio || encode_video) {
        compare_ts = av_compare_ts(video_stream->next_pts,
                                   video_stream->codec_ctx->time_base,
                                   audio_stream->next_pts,
                                   audio_stream->codec_ctx->time_base);
        if (encode_video && (compare_ts <= 0)) {
            video_count++;
            encode_video = !WriteVideoFrame(fmt_ctx, video_stream);
        } else {
            audio_count++;
            encode_audio = !WriteAudioFrame(fmt_ctx, audio_stream);
        }
    }

    if ((error_code = av_write_trailer(fmt_ctx)) < 0) {
        fprintf(stderr, "Failed to av_write_trailer: %s\n",
                ErrorToString(error_code));
        return error_code;
    }

end_mux_flv:
    if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&fmt_ctx->pb);
    }
    return error_code;
}

static int MultiplexFLV(const char *output_file) {
    int error_code = 0;
    AVFormatContext *fmt_ctx = nullptr;
    OutputStream video_stream{}, audio_stream{};

    // allocate AVFormatContext
    if ((error_code = avformat_alloc_output_context2(&fmt_ctx, nullptr, "flv",
                                                     output_file) < 0)) {
        fprintf(stderr, "Failed to avformat_alloc_output_context2: %s\n",
                ErrorToString(error_code));
        return error_code;
    }

    // find AVCodec
    const AVCodec *audio_codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!audio_codec) {
        fprintf(stderr, "Failed to find audio encoder\n");
        avformat_free_context(fmt_ctx);
        return -1;
    }
    const AVCodec *video_codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!video_codec) {
        fprintf(stderr, "Failed to find video encoder\n");
        avformat_free_context(fmt_ctx);
        return -1;
    }

    // add AVStream
    audio_stream.stream = avformat_new_stream(fmt_ctx, nullptr);
    if (!audio_stream.stream) {
        fprintf(stderr, "Failed to allocate audio stream\n");
        avformat_free_context(fmt_ctx);
        return -1;
    }
    audio_stream.stream->id = static_cast<int>(fmt_ctx->nb_streams) - 1;
    video_stream.stream = avformat_new_stream(fmt_ctx, nullptr);
    if (!video_stream.stream) {
        fprintf(stderr, "Failed to allocate video stream\n");
        avformat_free_context(fmt_ctx);
        return -1;
    }
    video_stream.stream->id = static_cast<int>(fmt_ctx->nb_streams) - 1;

    // allocate AVCodecContext
    audio_stream.codec_ctx = avcodec_alloc_context3(audio_codec);
    if (!audio_stream.codec_ctx) {
        fprintf(stderr, "Failed to allocate audio codec context\n");
        error_code = -1;
        goto close_streams;
    }
    video_stream.codec_ctx = avcodec_alloc_context3(video_codec);
    if (!video_stream.codec_ctx) {
        fprintf(stderr, "Failed to allocate video codec context\n");
        error_code = -1;
        goto close_streams;
    }
    if (fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
        // mkv flv mp4, no SPS/PPS before i frame
        audio_stream.codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        video_stream.codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    // open audio codec
    av_channel_layout_default(&audio_stream.codec_ctx->ch_layout,
                              kAudioChannels);
    audio_stream.codec_ctx->sample_fmt = kAudioSampleFormat;
    audio_stream.codec_ctx->profile = kAudioProfile;
    audio_stream.codec_ctx->sample_rate = kAudioSampleRate;
    audio_stream.codec_ctx->bit_rate = kAudioBitrate;
    audio_stream.stream->time_base = AVRational(1, kAudioSampleRate);
    if ((error_code = avcodec_open2(audio_stream.codec_ctx, audio_codec,
                                    nullptr)) < 0) {
        fprintf(stderr, "Failed to avcodec_open2: %s\n",
                ErrorToString(error_code));
        goto close_streams;
    }

    // open video codec
    video_stream.codec_ctx->width = kVideoWidth;
    video_stream.codec_ctx->height = kVideoHeight;
    video_stream.codec_ctx->time_base = AVRational(1, kVideoFrameRate);
    video_stream.codec_ctx->framerate = AVRational(kVideoFrameRate, 1);
    video_stream.codec_ctx->gop_size = kVideoFrameRate;
    video_stream.codec_ctx->max_b_frames = 0;
    video_stream.codec_ctx->pix_fmt = kVideoPixelFormat;
    video_stream.codec_ctx->bit_rate = kVideoBitrate;
    video_stream.stream->time_base = video_stream.codec_ctx->time_base;
    if ((error_code = avcodec_open2(video_stream.codec_ctx, video_codec,
                                    nullptr)) < 0) {
        fprintf(stderr, "Failed to avcodec_open2: %s\n",
                ErrorToString(error_code));
        goto close_streams;
    }

    // copy the stream parameters to the muxer
    if ((error_code =
             avcodec_parameters_from_context(audio_stream.stream->codecpar,
                                             audio_stream.codec_ctx) < 0)) {
        fprintf(stderr, "Could not copy the stream parameters: %s\n",
                ErrorToString(error_code));
        goto close_streams;
    }
    if ((error_code =
             avcodec_parameters_from_context(video_stream.stream->codecpar,
                                             video_stream.codec_ctx) < 0)) {
        fprintf(stderr, "Could not copy the stream parameters: %s\n",
                ErrorToString(error_code));
        goto close_streams;
    }

    // alloc AVFrame
    audio_stream.frame = av_frame_alloc();
    if (!audio_stream.frame) {
        fprintf(stderr, "Failed to allocate audio frame\n");
        error_code = -1;
        goto close_streams;
    }
    video_stream.frame = av_frame_alloc();
    if (!video_stream.frame) {
        fprintf(stderr, "Failed to allocate video frame\n");
        error_code = -1;
        goto close_streams;
    }

    av_dump_format(fmt_ctx, 0, output_file, 1);
    error_code = InnerMultiplexFLV(fmt_ctx, &audio_stream, &video_stream);

    close_streams:
    if (video_stream.frame) {
        av_frame_free(&video_stream.frame);
    }
    if (audio_stream.frame) {
        av_frame_free(&audio_stream.frame);
    }
    if (video_stream.codec_ctx) {
        avcodec_free_context(&video_stream.codec_ctx);
    }
    if (audio_stream.codec_ctx) {
        avcodec_free_context(&audio_stream.codec_ctx);
    }
    avformat_free_context(fmt_ctx);
    return error_code;
}

int main() {
    return MultiplexFLV("../../../../output.flv");
}
