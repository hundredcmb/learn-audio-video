extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <fstream>

thread_local static char error_buffer[AV_ERROR_MAX_STRING_SIZE] = {};
static constexpr int kDurationSeconds = 10;

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
    int64_t next_codec_pts;
    std::ifstream ifs;
};

static int FillYuv420pImage(AVFrame *frame, int frame_index,
                            int width, int height) {
    if (!frame || width <= 0 || height <= 0 || frame_index < 0) {
        return -1;
    }
    av_frame_unref(frame);

    int error_code = 0;
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = width;
    frame->height = height;
    if ((error_code = av_frame_get_buffer(frame, 0)) < 0) {
        fprintf(stderr, "Failed to av_frame_get_buffer(): %s\n",
                ErrorToString(error_code));
        return error_code;
    }

    int x, y, i;
    i = frame_index;
    /* Y */
    for (y = 0; y < height; y++)
        for (x = 0; x < width; x++)
            frame->data[0][y * frame->linesize[0] + x] = x + y + i * 3;

    /* Cb and Cr */
    for (y = 0; y < height / 2; y++) {
        for (x = 0; x < width / 2; x++) {
            frame->data[1][y * frame->linesize[1] + x] = 128 + y + i * 2;
            frame->data[2][y * frame->linesize[2] + x] = 64 + x + i * 5;
        }
    }

    return 0;
}

static int FillPcmSample(AVFrame *frame, AVCodecContext *codec_ctx,
                         std::ifstream &ifs) {
    if (!frame || !codec_ctx || !ifs) {
        fprintf(stderr, "Invalid argument\n");
        return -1;
    }
    av_frame_unref(frame);

    int error_code = 0;
    int bytes_per_sample = av_get_bytes_per_sample(codec_ctx->sample_fmt);
    if (bytes_per_sample <= 0) {
        fprintf(stderr, "Failed to get bytes per sample\n");
        return -1;
    }

    // allocate AVBufferRef[] according to the codec parameters
    frame->format = codec_ctx->sample_fmt;
    frame->ch_layout = codec_ctx->ch_layout;
    frame->nb_samples = codec_ctx->frame_size;
    frame->sample_rate = codec_ctx->sample_rate;
    if ((error_code = av_frame_get_buffer(frame, 0)) < 0) {
        fprintf(stderr, "Failed to allocate AVBufferRef[] in AVFrame: %s\n",
                ErrorToString(error_code));
        return error_code;
    }

    int nb_samples = frame->nb_samples;
    int nb_channels = frame->ch_layout.nb_channels;
    AVSampleFormat sample_fmt = codec_ctx->sample_fmt;
    int bytes_per_frame = bytes_per_sample * nb_channels * nb_samples;
    auto pcm_buffer_packed = std::make_unique<uint8_t[]>(bytes_per_frame);
    auto pcm_buffer_planar = std::make_unique<uint8_t[]>(bytes_per_frame);

    // read pcm samples
    std::memset(pcm_buffer_packed.get(), 0, bytes_per_frame);
    if (!ifs.read(reinterpret_cast<char *>(pcm_buffer_packed.get()),
                  bytes_per_frame)) {
        if (!ifs.eof()) {
            fprintf(stderr,
                    "Failed to read input file: ifstream is broken\n");
            return -1;
        }
    }
    int bytes_read = static_cast<int>(ifs.gcount());
    int samples_read = bytes_read / bytes_per_sample;
    int nb_samples_read = samples_read / nb_channels;
    if (nb_samples_read <= 0) {
        fprintf(stderr, "Failed to read input file: invalid samples\n");
        return -1;
    }

    // convert pcm sample format
    uint8_t *data = nullptr;
    if (av_sample_fmt_is_planar(sample_fmt)) {
        data = pcm_buffer_planar.get();
        std::memset(data, 0, bytes_per_frame);
        for (int i = 0; i < nb_channels; ++i) {
            for (int j = i; j < samples_read; j += nb_channels) {
                std::memcpy(data,
                            pcm_buffer_packed.get() + j * bytes_per_sample,
                            bytes_per_sample);
                data += bytes_per_sample;
            }
        }
        data = pcm_buffer_planar.get();
        for (int i = 0; i < nb_channels; ++i) {
            std::memcpy(frame->data[i],
                        data + i * nb_samples_read * bytes_per_sample,
                        nb_samples_read * bytes_per_sample);
        }
    } else {
        data = pcm_buffer_packed.get();
        std::memcpy(frame->data[0], data, bytes_read);
    }

    return nb_samples;
}

static int WriteVideoFrame(AVFormatContext *fmt_ctx,
                           OutputStream *v_stream) {
    if (v_stream->codec_ctx->pix_fmt != AV_PIX_FMT_YUV420P) {
        fprintf(stderr, "unsupported pixel format\n");
        return -1;
    }
    if (!v_stream->frame || !v_stream->packet) {
        fprintf(stderr, "frame or packet not alloc\n");
        return -1;
    }

    int error_code = 0;
    static int frame_index = 0;
    AVFrame *input_frame = v_stream->frame;

    // check duration
    int compare_ts = av_compare_ts(v_stream->next_codec_pts,
                                   v_stream->codec_ctx->time_base,
                                   kDurationSeconds,
                                   AVRational(1, 1));
    if (compare_ts >= 0) {
        // if stream end, drain the encoder
        input_frame = nullptr;
        printf("\nsend_video_frame: nullptr\n");
    } else {
        // if stream not end, init video frame
        error_code = FillYuv420pImage(v_stream->frame, frame_index,
                                      v_stream->codec_ctx->width,
                                      v_stream->codec_ctx->height);
        if (error_code < 0) {
            return -1;
        }
        v_stream->frame->pts = v_stream->next_codec_pts;
        printf("\nsend_video_frame: codec_pts=%lld\n", v_stream->frame->pts);
    }

    // send video frame to encoder
    if ((error_code = avcodec_send_frame(v_stream->codec_ctx,
                                         input_frame)) < 0) {
        if (error_code != AVERROR(EAGAIN) && error_code != AVERROR_EOF) {
            fprintf(stderr, "Failed to send packet to encoder: %s\n",
                    ErrorToString(error_code));
            return -1;
        }
    }

    // receive video packet from encoder
    while ((error_code = avcodec_receive_packet(v_stream->codec_ctx,
                                                v_stream->packet)) == 0) {
        v_stream->packet->stream_index = v_stream->stream->index;

        // rescale output packet timestamp values from codec to stream timebase
        av_packet_rescale_ts(v_stream->packet,
                             v_stream->codec_ctx->time_base,
                             v_stream->stream->time_base);

        printf("receive_video_packet: flv_pts=%lld\n",
               v_stream->packet->pts);

        // write packet to output
        error_code = av_interleaved_write_frame(fmt_ctx, v_stream->packet);
        if (error_code < 0) {
            fprintf(stderr, "Failed to write packet to output: %s\n",
                    ErrorToString(error_code));
            return -1;
        }
    }
    if (error_code != AVERROR(EAGAIN) && error_code != AVERROR_EOF) {
        fprintf(stderr, "Failed to receive frame from encoder: %s\n",
                ErrorToString(error_code));
        return -1;
    }
    if (compare_ts >= 0) {
        return 1;
    }

    v_stream->next_codec_pts += 1;
    frame_index += 1;
    return 0;
}

static int WriteAudioFrame(AVFormatContext *fmt_ctx,
                           OutputStream *a_stream) {
    if (!a_stream->frame || !a_stream->packet) {
        fprintf(stderr, "frame or packet not alloc\n");
        return -1;
    }

    int error_code = 0, nb_samples = 0;
    AVFrame *input_frame = a_stream->frame;
    int compare_ts = av_compare_ts(a_stream->next_codec_pts,
                                   a_stream->codec_ctx->time_base,
                                   kDurationSeconds,
                                   AVRational(1, 1));
    if (compare_ts >= 0) {
        // if stream end, drain the encoder
        input_frame = nullptr;
        printf("\nsend_audio_frame: nullptr\n");
    } else {
        // if stream not end, init audio frame
        nb_samples = FillPcmSample(a_stream->frame, a_stream->codec_ctx,
                                   a_stream->ifs);
        if (nb_samples < 0) {
            fprintf(stderr, "Failed to FillPcmSample\n");
            return -1;
        }
        a_stream->frame->pts = a_stream->next_codec_pts;
        printf("\nsend_audio_frame: codec_pts=%lld\n", a_stream->frame->pts);
    }

    // send audio frame to encoder
    if ((error_code = avcodec_send_frame(a_stream->codec_ctx,
                                         input_frame)) < 0) {
        if (error_code != AVERROR(EAGAIN) && error_code != AVERROR_EOF) {
            fprintf(stderr, "Failed to send packet to encoder: %s\n",
                    ErrorToString(error_code));
            return -1;
        }
    }

    // receive audio packet from encoder
    while ((error_code = avcodec_receive_packet(a_stream->codec_ctx,
                                                a_stream->packet)) == 0) {
        a_stream->packet->stream_index = a_stream->stream->index;

        // rescale output packet timestamp values from codec to stream timebase
        av_packet_rescale_ts(a_stream->packet,
                             a_stream->codec_ctx->time_base,
                             a_stream->stream->time_base);

        printf("receive_audio_packet: flv_pts=%lld\n",
               a_stream->packet->pts);

        // write packet to output
        error_code = av_interleaved_write_frame(fmt_ctx, a_stream->packet);
        if (error_code < 0) {
            fprintf(stderr, "Failed to write packet to output: %s\n",
                    ErrorToString(error_code));
            return -1;
        }
    }
    if (error_code != AVERROR(EAGAIN) && error_code != AVERROR_EOF) {
        fprintf(stderr, "Failed to receive frame from encoder: %s\n",
                ErrorToString(error_code));
        return -1;
    }
    if (compare_ts >= 0) {
        return 1;
    }

    a_stream->next_codec_pts += nb_samples;
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

    // write flv header
    // stream->time_base is changed
    // flv: audio=(1, 1000), video=(1, 1000); ts: audio=(1, 90000), video=(1, 90000)
    if ((error_code = avformat_write_header(fmt_ctx, nullptr) < 0)) {
        fprintf(stderr, "Failed to avformat_write_header: %s\n",
                ErrorToString(error_code));
        goto end_mux_flv;
    }

    while (encode_audio || encode_video) {
        compare_ts = av_compare_ts(video_stream->next_codec_pts,
                                   video_stream->codec_ctx->time_base,
                                   audio_stream->next_codec_pts,
                                   audio_stream->codec_ctx->time_base);
        if (encode_video && (compare_ts <= 0)) {
            if ((error_code = WriteVideoFrame(fmt_ctx, video_stream)) < 0) {
                break;
            }
            encode_video = !error_code;
        } else {
            if ((error_code = WriteAudioFrame(fmt_ctx, audio_stream)) < 0) {
                break;
            }
            encode_audio = !error_code;
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

static int MultiplexFLV(const char *output_file, const char *input_pcm_file) {
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
    audio_stream.stream->duration = av_rescale_q(kDurationSeconds,
                                                 AVRational(1, 1),
                                                 audio_stream.stream->time_base);
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
    video_stream.stream->duration = av_rescale_q(kDurationSeconds,
                                                 AVRational(1, 1),
                                                 video_stream.stream->time_base);
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

    // alloc AVPacket
    audio_stream.packet = av_packet_alloc();
    if (!audio_stream.packet) {
        fprintf(stderr, "Failed to allocate audio packet\n");
        error_code = -1;
        goto close_streams;
    }
    video_stream.packet = av_packet_alloc();
    if (!video_stream.packet) {
        fprintf(stderr, "Failed to allocate video packet\n");
        error_code = -1;
        goto close_streams;
    }

    // open input file
    audio_stream.ifs = std::ifstream(input_pcm_file,
                                     std::ios::in | std::ios::binary);
    if (!audio_stream.ifs.is_open()) {
        fprintf(stderr, "Failed to open input file: %s\n", input_pcm_file);
        error_code = -1;
        goto close_streams;
    }

    av_dump_format(fmt_ctx, 0, output_file, 1);
    error_code = InnerMultiplexFLV(fmt_ctx, &audio_stream, &video_stream);

close_streams:
    if (video_stream.packet) {
        av_packet_free(&video_stream.packet);
    }
    if (audio_stream.packet) {
        av_packet_free(&audio_stream.packet);
    }
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
    // ffmpeg -i yuv420p_640x360_25fps.mp4 -ar 48000 -ac 2 -f f32le 48k_f32le_2ch.pcm
    return MultiplexFLV("../../../../output.flv",
                        "../../../../48k_f32le_2ch.pcm");
    // ffplay output.flv -autoexit
}
