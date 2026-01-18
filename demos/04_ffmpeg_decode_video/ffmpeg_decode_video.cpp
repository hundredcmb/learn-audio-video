#if 1
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
}

#include <fstream>

static constexpr std::size_t kInputVideoBufferSize = 20480;
static constexpr int kInputVideoBufferRefillThreshold = 4096;
thread_local static char error_buffer[AV_ERROR_MAX_STRING_SIZE] = {}; // store FFmpeg error string

static char *ErrorToString(const int error_code) {
    std::memset(error_buffer, 0, AV_ERROR_MAX_STRING_SIZE);
    return av_make_error_string(error_buffer, AV_ERROR_MAX_STRING_SIZE, error_code);
}

static std::string GetFileExtension(std::string_view file_name) {
    size_t pos = file_name.rfind('.');
    if (pos == std::string::npos) {
        return "";
    }
    std::string extension(file_name.substr(pos + 1));
    for (char &c: extension) {
        c = static_cast<char>(std::tolower(c));
    }
    return extension;
}

static bool InnerDecodeVideo(AVCodecContext *codec_ctx, AVPacket *pkt, std::ofstream &ofs) {
    if (!codec_ctx || !pkt) {
        return false;
    }

    int error_code{};
    bool logged = false;

    // send packet to decoder
    if ((error_code = avcodec_send_packet(codec_ctx, pkt)) < 0) {
        if (error_code != AVERROR(EAGAIN) && error_code != AVERROR_EOF) {
            fprintf(stderr, "Failed to send packet to decoder: %s\n", ErrorToString(error_code));
            return false;
        }
    }

    // allocate AVFrame
    AVFrame *frame = av_frame_alloc();
    if (frame == nullptr) {
        fprintf(stderr, "Failed to allocate AVFrame: av_frame_alloc()\n");
        return false;
    }

    // receive pixel data from decoder, until EOF
    // do not need to manage pixel data memory
    while ((error_code = avcodec_receive_frame(codec_ctx, frame)) == 0) {
        AVPixelFormat pix_fmt = codec_ctx->pix_fmt;

        // log 1 time per frame
        if (!logged) {
            if (pix_fmt != AV_PIX_FMT_YUV420P) {
                fprintf(stderr, "Unsupported pixel format: %s\n", av_get_pix_fmt_name(pix_fmt));
                continue;
            }
            printf("Decode %dB AVPacket, %dx%d, pix_fmt=%s\n",
                   pkt->size, frame->width, frame->height, av_get_pix_fmt_name(pix_fmt));
            logged = true;
        }

        if (!ofs) {
            continue;
        }

        // write to output file
        if (pix_fmt == AV_PIX_FMT_YUV420P) {
            for (int i = 0; (i < frame->height && ofs); ++i) {
                ofs.write(reinterpret_cast<char *>(frame->data[0] + i * frame->linesize[0]), frame->width);
            }
            for (int i = 0; (i < frame->height / 2 && ofs); ++i) {
                ofs.write(reinterpret_cast<char *>(frame->data[1] + i * frame->linesize[1]), frame->width / 2);
            }
            for (int i = 0; (i < frame->height / 2 && ofs); ++i) {
                ofs.write(reinterpret_cast<char *>(frame->data[2] + i * frame->linesize[2]), frame->width / 2);
            }
        }
        if (!ofs) {
            fprintf(stderr, "Failed to write yuv file, ofstream is broken\n");
        }
    }

    av_frame_free(&frame);

    if (error_code != AVERROR(EAGAIN) && error_code != AVERROR_EOF) {
        fprintf(stderr, "Failed to receive frame from decoder: %s\n", ErrorToString(error_code));
        return false;
    }

    if (!ofs) {
        return false;
    }

    return true;
}

void DecodeVideo(const char *input_file, const char *output_file) {
    int error_code{};

    // check file extension
    AVCodecID codec_id{};
    std::string file_extension = GetFileExtension(input_file);
    if (file_extension == "h264") {
        codec_id = AV_CODEC_ID_H264;
        printf("Decode H264 video start\n");
    } else {
        fprintf(stderr, "Unsupported video format: %s\n", file_extension.c_str());
        return;
    }

    // find AVCodec
    const AVCodec *codec = avcodec_find_decoder(codec_id);
    if (codec == nullptr) {
        fprintf(stderr, "AVCodec not found: %d\n", codec_id);
        return;
    }

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

    // initialize AVCodecParserContext
    AVCodecParserContext *parser_ctx = av_parser_init(codec->id);
    if (parser_ctx == nullptr) {
        fprintf(stderr, "Failed to init AVCodecParserContext: %d\n", codec->id);
        return;
    }

    // allocate AVCodecContext
    AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
    if (codec_ctx == nullptr) {
        fprintf(stderr, "Failed to allocate AVCodecContext: %d\n", codec->id);
        av_parser_close(parser_ctx);
        return;
    }

    // initialize AVCodecContext
    if ((error_code = avcodec_open2(codec_ctx, codec, nullptr)) < 0) {
        fprintf(stderr, "Failed to init AVCodecContext: %s\n", ErrorToString(error_code));
        avcodec_free_context(&codec_ctx);
        av_parser_close(parser_ctx);
        return;
    }

    // allocate AVPacket
    AVPacket *pkt = av_packet_alloc();
    if (pkt == nullptr) {
        fprintf(stderr, "Failed to allocate AVPacket: av_packet_alloc()\n");
        avcodec_free_context(&codec_ctx);
        av_parser_close(parser_ctx);
        return;
    }

    // allocate input buffer
    const std::size_t input_buffer_size = kInputVideoBufferSize + AV_INPUT_BUFFER_PADDING_SIZE;
    auto input_buffer = std::make_unique<uint8_t[]>(input_buffer_size); // std=c++17
    std::memset(input_buffer.get(), 0, input_buffer_size);
    uint8_t *data = input_buffer.get();

    size_t data_size{};
    while (true) {
        // refill input buffer
        if (data_size < kInputVideoBufferRefillThreshold && !ifs.eof()) {
            if (data_size > 0) {
                std::memcpy(input_buffer.get(), data, data_size);
            }
            data = input_buffer.get();
            std::size_t bytes_to_read = kInputVideoBufferRefillThreshold - data_size;
            if (!ifs.read(reinterpret_cast<char *>(data) + data_size, static_cast<std::streamsize>(bytes_to_read))) {
                if (!ifs.eof()) {
                    fprintf(stderr, "Failed to read input file: %s\n", input_file);
                    break;
                }
                fprintf(stderr, "End of ifstream: %s\n", input_file);
            }
            data_size += ifs.gcount();
        }

        // parse an video frame. if success, pkt->data == data && pkt->size == parsed
        int parsed = av_parser_parse2(parser_ctx, codec_ctx,
                                      &pkt->data, &pkt->size,
                                      data, static_cast<int>(data_size),
                                      AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (parsed < 0) {
            fprintf(stderr, "Failed to parse video: %s\n", ErrorToString(parsed));
            break;
        }
        data += parsed;
        data_size -= parsed;

        // decode video and write to output_file
        if (pkt->size > 0) {
            InnerDecodeVideo(codec_ctx, pkt, ofs);
        }

        // if decode end, drain the decoder
        if (data_size == 0 && ifs.eof()) {
            pkt->data = nullptr;
            pkt->size = 0;
            InnerDecodeVideo(codec_ctx, pkt, ofs);
            break;
        }
    }

    printf("Decode H264 video end\n");

    av_packet_free(&pkt);
    avcodec_free_context(&codec_ctx);
    av_parser_close(parser_ctx);
}

int main(int argc, char *argv[]) {
    // ffmpeg -i yuv420p_640x360_25fps.mp4 -an -c:v copy yuv420p_640x360_25fps.h264
    // ffplay -pixel_format yuv420p -video_size 640x360 -framerate 25 yuv420p_640x360_25fps.yuv
    DecodeVideo("../../../../yuv420p_640x360_25fps.h264", "../../../../yuv420p_640x360_25fps.yuv");
}
#endif

#if 0
extern "C" {
#include <libavcodec/bsf.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <fstream>

thread_local static char error_buffer[AV_ERROR_MAX_STRING_SIZE] = {};

static char *ErrorToString(const int error_code) {
    std::memset(error_buffer, 0, AV_ERROR_MAX_STRING_SIZE);
    return av_make_error_string(error_buffer, AV_ERROR_MAX_STRING_SIZE, error_code);
}

static bool InnerExtractVideoStreamAnnexB(AVFormatContext *fmt_ctx, AVPacket *pkt, std::ofstream &ofs) {
    if (!fmt_ctx || !pkt || !ofs) {
        return false;
    }

    int error_code{};
    int h264_stream_index = -1;

    // find h264 video stream
    AVStream *stream = nullptr;
    AVCodecParameters *codec_params = nullptr;
    for (int i = 0; i < fmt_ctx->nb_streams; i++) {
        stream = fmt_ctx->streams[i];
        codec_params = stream->codecpar;
        if (codec_params->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (codec_params->codec_id == AV_CODEC_ID_H264) {
                h264_stream_index = i;
                break;
            }
        }
    }
    if (h264_stream_index < 0) {
        fprintf(stderr, "Could not find H264 video stream\n");
        return false;
    }

    // allocate bitstream filter context
    const AVBitStreamFilter *bs_filter = av_bsf_get_by_name("h264_mp4toannexb");
    if (bs_filter == nullptr) {
        fprintf(stderr, "Could not find h264_mp4toannexb bitstream filter\n");
        return false;
    }
    AVBSFContext *bsf_ctx = nullptr;
    if ((error_code = av_bsf_alloc(bs_filter, &bsf_ctx)) < 0) {
        fprintf(stderr, "Could not allocate bitstream filter context: %s\n", ErrorToString(error_code));
        return false;
    }

    // initialize bitstream filter context
    if ((error_code = avcodec_parameters_copy(bsf_ctx->par_in, codec_params)) < 0) {
        fprintf(stderr, "Could not copy codec parameters: %s\n", ErrorToString(error_code));
        av_bsf_free(&bsf_ctx);
        return false;
    }
    if ((error_code = av_bsf_init(bsf_ctx)) < 0) {
        fprintf(stderr, "Could not initialize bitstream filter context: %s\n", ErrorToString(error_code));
        av_bsf_free(&bsf_ctx);
        return false;
    }

    while (true) {
        if ((error_code = av_read_frame(fmt_ctx, pkt)) < 0) {
            if (error_code != AVERROR_EOF) {
                fprintf(stderr, "Could not read frame: %s\n", ErrorToString(error_code));
            } else {
                printf("End of input file\n");
            }
            break;
        }
        if (pkt->stream_index != h264_stream_index) {
            av_packet_unref(pkt);
            continue;
        }

        // send packet to bitstream filter
        if ((error_code = av_bsf_send_packet(bsf_ctx, pkt)) < 0) {
            fprintf(stderr, "Could not send packet to bitstream filter: %s\n", ErrorToString(error_code));
            av_packet_unref(pkt);
            continue;
        }
        av_packet_unref(pkt);

        // receive packet from bitstream filter
        while ((error_code = av_bsf_receive_packet(bsf_ctx, pkt)) == 0) {
            if (!ofs.write(reinterpret_cast<const char *>(pkt->data), pkt->size)) {
                fprintf(stderr, "Could not write ha64 file: ofstream is broken\n");
                av_packet_unref(pkt);
                av_bsf_free(&bsf_ctx);
                return false;
            }
            printf("Extracted %ld bytes of H264 data\n", pkt->size);
            av_packet_unref(pkt);
        }
        if (error_code != AVERROR(EAGAIN)) {
            av_packet_unref(pkt);
            fprintf(stderr, "Could not receive packet from bitstream filter: %s\n", ErrorToString(error_code));
        }
    }

    av_bsf_free(&bsf_ctx);
    return true;
}

void ExtractVideoStreamAnnexB(const char *input_file, const char *output_file) {
    int error_code{};

    // open input_file
    AVFormatContext *fmt_ctx = nullptr;
    if ((error_code = avformat_open_input(&fmt_ctx, input_file, nullptr, nullptr)) < 0) {
        fprintf(stderr, "Could not open source file '%s': %s\n", input_file, ErrorToString(error_code));
        return;
    }
    if ((error_code = avformat_find_stream_info(fmt_ctx, nullptr)) < 0) {
        fprintf(stderr, "Could not find stream information: %s\n", ErrorToString(error_code));
        avformat_close_input(&fmt_ctx);
        return;
    }

    // open output_file
    std::ofstream ofs(output_file, std::ios::out | std::ios::binary);
    if (!ofs.is_open()) {
        fprintf(stderr, "Could not open output file '%s'\n", output_file);
        avformat_close_input(&fmt_ctx);
        return;
    }

    // allocate AVPacket
    AVPacket *pkt = nullptr;
    if ((pkt = av_packet_alloc()) == nullptr) {
        fprintf(stderr, "Could not allocate AVPacket: av_packet_alloc()\n");
        avformat_close_input(&fmt_ctx);
        return;
    }

    InnerExtractVideoStreamAnnexB(fmt_ctx, pkt, ofs);

    ofs.close();
    av_packet_free(&pkt);
    avformat_close_input(&fmt_ctx);
}

int main() {
    ExtractVideoStreamAnnexB("../../../../yuv420p_640x360_25fps.mp4", "../../../../yuv420p_640x360_25fps.h264");
    return 0;
}
#endif
