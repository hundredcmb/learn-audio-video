extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <fstream>

static constexpr std::size_t kInputAudioBufferSize = 20480;
static constexpr int kInputAudioBufferRefillThreshold = 4096;
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

static bool InnerDecodeAudio(AVCodecContext *codec_ctx, AVPacket *pkt, std::ofstream &ofs) {
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

    // receive pcm data from decoder, until EOF
    // do not need to manage pcm data memory
    while ((error_code = avcodec_receive_frame(codec_ctx, frame)) == 0) {
        int is_planar = av_sample_fmt_is_planar(codec_ctx->sample_fmt);

        // log 1 time per frame
        if (!logged) {
            printf("Decode a %d bytes AAC frame, sample_rate=%d, channels=%d, sample_format=%d, is_planar=%d\n",
                   pkt->size, codec_ctx->sample_rate, codec_ctx->ch_layout.nb_channels, codec_ctx->sample_fmt,
                   is_planar);
            logged = true;
        }

        if (!ofs) {
            continue;
        }

        int bytes_per_sample = av_get_bytes_per_sample(codec_ctx->sample_fmt);
        if (bytes_per_sample <= 0) {
            fprintf(stderr, "Failed to get bytes per sample: %d\n", codec_ctx->sample_fmt);
            continue;
        }

        // write to output file
        // if planar format: LL...LLRR...RR, L in data[0], R in data[1]
        // if packed format: LRLR...LRLR, LR in data[0]
        // output format only support packed
        if (is_planar) {
            for (int i = 0; i < frame->nb_samples; ++i) {
                for (int j = 0; j < codec_ctx->ch_layout.nb_channels; ++j) {
                    if (!ofs.write(reinterpret_cast<char *>(frame->data[j] + i * bytes_per_sample), bytes_per_sample)) {
                        fprintf(stderr, "Failed to write pcm file, ofstream is broken\n");
                        continue;
                    }
                }
            }
        } else {
            if (!ofs.write(reinterpret_cast<char *>(frame->data[0]),
                           frame->nb_samples * bytes_per_sample * codec_ctx->ch_layout.nb_channels)) {
                fprintf(stderr, "Failed to write pcm file, ofstream is broken\n");
                continue;
            }
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

void DecodeAudio(const char *input_file, const char *output_file) {
    int error_code{};

    // check file extension
    AVCodecID codec_id{};
    std::string file_extension = GetFileExtension(input_file);
    if (file_extension == "aac") {
        codec_id = AV_CODEC_ID_AAC;
        printf("Decode AAC audio start\n");
    } else {
        fprintf(stderr, "Unsupported audio format: %s\n", file_extension.c_str());
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
    const std::size_t input_buffer_size = kInputAudioBufferSize + AV_INPUT_BUFFER_PADDING_SIZE;
    auto input_buffer = std::make_unique<uint8_t[]>(input_buffer_size); // std=c++17
    std::memset(input_buffer.get(), 0, input_buffer_size);
    uint8_t *data = input_buffer.get();

    size_t data_size{};
    while (true) {
        // refill input buffer
        if (data_size < kInputAudioBufferRefillThreshold && !ifs.eof()) {
            if (data_size > 0) {
                std::memcpy(input_buffer.get(), data, data_size);
            }
            data = input_buffer.get();
            std::size_t bytes_to_read = kInputAudioBufferSize - data_size;
            if (!ifs.read(reinterpret_cast<char *>(data) + data_size, static_cast<std::streamsize>(bytes_to_read))) {
                if (!ifs.eof()) {
                    fprintf(stderr, "Failed to read input file: %s\n", input_file);
                    break;
                }
                fprintf(stderr, "End of ifstream: %s\n", input_file);
            }
            data_size += ifs.gcount();
        }

        // parse an audio frame. if success, pkt->data == data && pkt->size == parsed
        int parsed = av_parser_parse2(parser_ctx, codec_ctx,
                                      &pkt->data, &pkt->size,
                                      data, static_cast<int>(data_size),
                                      AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (parsed < 0) {
            fprintf(stderr, "Failed to parse audio: %s\n", ErrorToString(parsed));
            break;
        }
        data += parsed;
        data_size -= parsed;

        // decode audio and write to output_file
        if (pkt->size > 0) {
            InnerDecodeAudio(codec_ctx, pkt, ofs);
        }

        // if decode end, drain the decoder
        if (data_size == 0 && ifs.eof()) {
            pkt->data = nullptr;
            pkt->size = 0;
            InnerDecodeAudio(codec_ctx, pkt, ofs);
            break;
        }
    }

    printf("Decode AAC audio end\n");

    av_packet_free(&pkt);
    avcodec_free_context(&codec_ctx);
    av_parser_close(parser_ctx);
}

int main(int argc, char *argv[]) {
    // ffmpeg -i yuv420p_640x360_25fps.mp4 -vn -c:a copy 48k_f32le_2ch.aac
    DecodeAudio("../../../../48k_f32le_2ch.aac", "../../../../48k_f32le_2ch.pcm");
    // ffplay -ar 48000 -ac 2 -f f32le 48k_f32le_2ch.pcm
    return 0;
}
