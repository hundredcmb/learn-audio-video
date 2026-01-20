extern "C" {
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <chrono>
#include <fstream>

thread_local static char error_buffer[AV_ERROR_MAX_STRING_SIZE] = {}; // store FFmpeg error string

static char *ErrorToString(const int error_code) {
    std::memset(error_buffer, 0, AV_ERROR_MAX_STRING_SIZE);
    return av_make_error_string(error_buffer, AV_ERROR_MAX_STRING_SIZE, error_code);
}

static bool EncodeAndWrite(AVCodecContext *codec_ctx, AVFrame *frame, AVPacket *pkt, std::ofstream &ofs) {
    if (!codec_ctx || !pkt || !ofs) {
        return false;
    }
    int error_code{};

    // send yuv frame to encoder
    if ((error_code = avcodec_send_frame(codec_ctx, frame)) < 0) {
        if (error_code != AVERROR(EAGAIN) && error_code != AVERROR_EOF) {
            fprintf(stderr, "Failed to send packet to encoder: %s\n", ErrorToString(error_code));
            return false;
        }
    }

    // receive avc from encoder, until EOF
    // do not need to manage aac memory
    while ((error_code = avcodec_receive_packet(codec_ctx, pkt)) == 0) {
        if (!ofs) {
            continue;
        }
        ofs.write(reinterpret_cast<char *>(pkt->data), pkt->size);
    }
    if (error_code != AVERROR(EAGAIN) && error_code != AVERROR_EOF) {
        fprintf(stderr, "Failed to receive frame from encoder: %s\n", ErrorToString(error_code));
        return false;
    }

    if (!ofs) {
        fprintf(stderr, "Failed to write aac file, ofstream is broken\n");
        return false;
    }
    return true;
}

static bool InnerEncodeVideoAVC(AVCodecContext *codec_ctx, AVFrame *frame, std::ifstream &ifs, std::ofstream &ofs) {
    if (!codec_ctx || !frame || !ifs || !ofs) {
        return false;
    }

    int error_code = 0;

    // allocate AVBufferRef[] according to the codec parameters
    frame->format = codec_ctx->pix_fmt;
    frame->width = codec_ctx->width;
    frame->height = codec_ctx->height;
    if ((error_code = av_frame_get_buffer(frame, 0)) < 0) {
        fprintf(stderr, "Failed to allocate AVBufferRef[] in AVFrame: %s\n", ErrorToString(error_code));
        return false;
    }

    // allocate AVPacket
    AVPacket *pkt = av_packet_alloc();
    if (pkt == nullptr) {
        fprintf(stderr, "Failed to allocate AVPacket: av_packet_alloc()\n");
        return false;
    }

    bool ret = true;
    int64_t pts = 0;
    int frame_bytes = av_image_get_buffer_size(codec_ctx->pix_fmt, frame->width, frame->height, 1);
    if (frame_bytes < 0) {
        fprintf(stderr, "Failed to get frame buffer size: %s\n", ErrorToString(frame_bytes));
        av_packet_free(&pkt);
        return false;
    }
    auto yuv_buffer = std::make_unique<uint8_t[]>(frame_bytes);

    while (true) {
        std::memset(yuv_buffer.get(), 0, frame_bytes);
        if (!ifs.read(reinterpret_cast<char *>(yuv_buffer.get()), frame_bytes)) {
            if (!ifs.eof()) {
                fprintf(stderr, "Failed to read input file: ifstream is broken\n");
                ret = false;
                break;
            }
        }
        int bytes_read = static_cast<int>(ifs.gcount());
        if (bytes_read <= 0) {
            break;
        }

        // initialize AVFrame
        if ((error_code = av_frame_make_writable(frame)) < 0) {
            fprintf(stderr, "Failed to make AVFrame writable: %s\n", ErrorToString(error_code));
            ret = false;
            break;
        }
        if ((error_code = av_image_fill_arrays(frame->data, frame->linesize, yuv_buffer.get(), codec_ctx->pix_fmt, frame->width, frame->height, 1)) < 0) {
            fprintf(stderr, "Failed to fill AVFrame data: %s\n", ErrorToString(error_code));
            ret = false;
            break;
        }
        pts += 1;
        frame->pts = pts;

        // encode video, write to file
        if (!EncodeAndWrite(codec_ctx, frame, pkt, ofs)) {
            ret = false;
            break;
        }

        if (ifs.eof()) {
            break;
        }
    }

    // if encode end, drain the encoder
    if (!EncodeAndWrite(codec_ctx, nullptr, pkt, ofs)) {
        ret = false;
    }

    av_packet_free(&pkt);
    return ret;
}

static void EncodeVideoAVC(int width, int height, int frame_rate, int64_t bit_rate, AVPixelFormat pixel_format,
                           const char *codec_name, const char *input_file, const char *output_file) {
    int error_code = 0;

    // find AVCodec, default libx264 encoder
    const AVCodec *codec = avcodec_find_encoder_by_name(codec_name);
    if (codec == nullptr) {
        fprintf(stderr, "AVCodec '%s' not found, use libx264\n", codec_name);
        codec_name = "libx264";
        codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!codec) {
            fprintf(stderr, "AVCodec '%s' not found\n", codec_name);
            return;
        }
    }
    printf("AVCodec found '%s'\n", codec_name);

    // open input_file and output_file
    std::ifstream ifs(input_file, std::ios::in | std::ios::binary);
    if (!ifs.is_open()) {
        fprintf(stderr, "Failed to open input file: %s\n", input_file);
        return;
    }
    std::ofstream ofs(output_file, std::ios::out | std::ios::binary);
    if (!ofs.is_open()) {
        fprintf(stderr, "Failed to open output file: %s\n", output_file);
        return;
    }

    // allocate AVCodecContext
    AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
    if (codec_ctx == nullptr) {
        fprintf(stderr, "Failed to allocate AVCodecContext: %d\n", codec->id);
        return;
    }

    codec_ctx->width = width;
    codec_ctx->height = height;
    codec_ctx->time_base = AVRational(1, frame_rate);
    codec_ctx->framerate = AVRational(frame_rate, 1);
    codec_ctx->gop_size = frame_rate;
    codec_ctx->max_b_frames = 0;
    codec_ctx->pix_fmt = pixel_format;
    codec_ctx->bit_rate = bit_rate;

    // ffmpeg -h encoder=libx264; x264 --fullhelp
    if (codec->id == AV_CODEC_ID_H264) {
        if ((error_code = av_opt_set(codec_ctx->priv_data, "preset", "veryslow", 0)) < 0) {
            fprintf(stderr, "Failed to set libx264 --preset: %s\n", ErrorToString(error_code));
        }
        if ((error_code = av_opt_set(codec_ctx->priv_data, "profile", "high", 0)) < 0) {
            fprintf(stderr, "Failed to set libx264 --profile: %s\n", ErrorToString(error_code));
        }
        if ((error_code = av_opt_set(codec_ctx->priv_data, "tune", "film", 0)) < 0) {
            fprintf(stderr, "Failed to set libx264 --tune: %s\n", ErrorToString(error_code));
        }
    }

    // initialize AVCodec
    if ((error_code = avcodec_open2(codec_ctx, codec, nullptr)) < 0) {
        fprintf(stderr, "Failed to init AVCodecContext: %s\n", ErrorToString(error_code));
        avcodec_free_context(&codec_ctx);
        return;
    }
    printf("AVCodec '%s' initialized\n", codec_name);

    // allocate AVFrame
    AVFrame *frame = av_frame_alloc();
    if (frame == nullptr) {
        fprintf(stderr, "Failed to allocate AVFrame: av_frame_alloc()\n");
        avcodec_free_context(&codec_ctx);
        return;
    }

    printf("Start to encode video\n");
    auto start = std::chrono::high_resolution_clock::now();
    InnerEncodeVideoAVC(codec_ctx, frame, ifs, ofs);
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    printf("End of encode video, cost %lld ms\n", duration);

    av_frame_free(&frame);
    avcodec_free_context(&codec_ctx);
}

int main() {
    // ffmpeg -i yuv420p_640x360_25fps.mp4 -an -c:v rawvideo -pix_fmt yuv420p yuv420p_640x360_25fps.yuv
    EncodeVideoAVC(640, 360, 25, 1000000, AV_PIX_FMT_YUV420P, "libx264", "../../../../yuv420p_640x360_25fps.yuv",
                   "../../../../yuv420p_640x360_25fps.h264");
    // ffplay yuv420p_640x360_25fps.h264
    return 0;
}
