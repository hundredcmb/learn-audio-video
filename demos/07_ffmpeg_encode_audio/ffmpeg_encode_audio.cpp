extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <chrono>
#include <fstream>
#include <sstream>
#include <unordered_map>

static constexpr int ADTS_HEADER_LEN = 7;
static constexpr int kDefaultProfile = FF_PROFILE_AAC_LOW;
thread_local static char error_buffer[AV_ERROR_MAX_STRING_SIZE] = {}; // store FFmpeg error string

static const std::unordered_map<int, int> sampling_frequency_map = {
    {96000, 0x0}, {88200, 0x1}, {64000, 0x2}, {48000, 0x3}, {44100, 0x4},
    {32000, 0x5}, {24000, 0x6}, {22050, 0x7}, {16000, 0x8}, {12000, 0x9},
    {11025, 0xa}, {8000,  0xb}, {7350,  0xc}
};

bool GenerateHeaderADTS(uint8_t *adts_header_buf, int data_len, int profile, int sample_rate, int nb_channels) {
    int sampling_frequency_index{};
    uint32_t aac_frame_length = data_len + ADTS_HEADER_LEN;
    auto it = sampling_frequency_map.find(sample_rate);
    if (it == sampling_frequency_map.end()) {
        return false;
    }
    sampling_frequency_index = it->second;
    // 12bits syncword: 0xfff
    adts_header_buf[0] = 0xff;
    adts_header_buf[1] = 0xf0;
    // 1bit ID: 0 for MPEG-4, 1 for MPEG-2
    adts_header_buf[1] |= (0 << 3);
    // 2bits layer: 0
    adts_header_buf[1] |= (0 << 1);
    // 1bit protection absent: set to 1 if there is no CRC and 0 if there is CRC
    adts_header_buf[1] |= 1;
    // 2bits profile
    adts_header_buf[2] = (profile) << 6;
    // 4bits sampling_frequency_index
    adts_header_buf[2] |= (sampling_frequency_index & 0x0f) << 2;
    // 1bit private bit: 0
    adts_header_buf[2] |= (0 << 1);
    // 3bits channel_configuration
    adts_header_buf[2] |= (nb_channels & 0x04) >> 2;
    adts_header_buf[3] = (nb_channels & 0x03) << 6;
    // 1bit original_copy: 0
    adts_header_buf[3] |= (0 << 5);
    // 1bit home: 0
    adts_header_buf[3] |= (0 << 4);
    // 1bit copyright_identification_bit: 0
    adts_header_buf[3] |= (0 << 3);
    // 1bit copyright_identification_start: 0
    adts_header_buf[3] |= (0 << 2);
    // 13bits aac_frame_length
    adts_header_buf[3] |= ((aac_frame_length & 0x1800) >> 11);
    adts_header_buf[4] = (uint8_t) ((aac_frame_length & 0x7f8) >> 3);
    adts_header_buf[5] = (uint8_t) ((aac_frame_length & 0x7) << 5);
    // 11bits adts_buffer_fullness: 0x7ff for variable bitrate stream
    adts_header_buf[5] |= 0x1f;
    adts_header_buf[6] = 0xfc;
    // 2bits number_of_raw_data_blocks_in_frame: 0
    // it indicates that there are 0+1 AAC original frames in the ADTS frame
    adts_header_buf[6] |= 0x0;
    return true;
}

static char *ErrorToString(const int error_code) {
    std::memset(error_buffer, 0, AV_ERROR_MAX_STRING_SIZE);
    return av_make_error_string(error_buffer, AV_ERROR_MAX_STRING_SIZE, error_code);
}

static bool SetSampleFormat(const AVCodec *codec, AVCodecContext *codec_ctx, const AVSampleFormat sample_fmt) {
    if (!codec || !codec_ctx) {
        return false;
    }

    // get supported sample formats and traverse
    int nb_sample_fmts{};
    const void *sample_fmts = nullptr;
    avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &sample_fmts, &nb_sample_fmts);
    for (int i = 0; i < nb_sample_fmts; ++i) {
        if (sample_fmt == *(static_cast<const AVSampleFormat *>(sample_fmts) + i)) {
            codec_ctx->sample_fmt = sample_fmt;
            return true;
        }
    }

    fprintf(stderr, "Specified sample format '%s' is not supported by the '%s' encoder\n", av_get_sample_fmt_name(sample_fmt), avcodec_get_name(codec->id));
    std::ostringstream oss;
    for (int i = 0; i < nb_sample_fmts; ++i) {
        oss << av_get_sample_fmt_name(*(static_cast<const AVSampleFormat *>(sample_fmts) + i)) << " ";
    }
    fprintf(stderr, "Supported sample formats: %s\n", oss.str().c_str());
    return false;
}

static bool SetSampleRate(const AVCodec *codec, AVCodecContext *codec_ctx, int sample_rate) {
    if (!codec || !codec_ctx) {
        return false;
    }

    // get supported sample rates and traverse
    int nb_sample_rates{};
    const void *sample_rates = nullptr;
    avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_RATE, 0, &sample_rates, &nb_sample_rates);
    for (int i = 0; i < nb_sample_rates; ++i) {
        if (sample_rate == *(static_cast<const int *>(sample_rates) + i)) {
            codec_ctx->sample_rate = sample_rate;
            return true;
        }
    }

    fprintf(stderr, "Specified sample rate %d is not supported by the '%s' encoder\n", sample_rate, avcodec_get_name(codec->id));
    std::ostringstream oss;
    for (int i = 0; i < nb_sample_rates; ++i) {
        oss << *(static_cast<const int *>(sample_rates) + i) << ", ";
    }
    fprintf(stderr, "Supported sample rates: %s\n", oss.str().c_str());
    return false;
}

static bool EncodeAndWrite(AVCodecContext *codec_ctx, AVFrame *frame, AVPacket *pkt, std::ofstream &ofs) {
    if (!codec_ctx || !pkt || !ofs) {
        return false;
    }

    int error_code{};
    int profile = codec_ctx->profile;
    int sample_rate = codec_ctx->sample_rate;
    int nb_channels = codec_ctx->ch_layout.nb_channels;

    // send pcm to encoder
    if ((error_code = avcodec_send_frame(codec_ctx, frame)) < 0) {
        if (error_code != AVERROR(EAGAIN) && error_code != AVERROR_EOF) {
            fprintf(stderr, "Failed to send packet to encoder: %s\n", ErrorToString(error_code));
            return false;
        }
    }

    // receive aac from encoder, until EOF
    // do not need to manage aac memory
    while ((error_code = avcodec_receive_packet(codec_ctx, pkt)) == 0) {
        if (!ofs) {
            continue;
        }
        uint8_t adts_header[ADTS_HEADER_LEN];
        if (GenerateHeaderADTS(adts_header, pkt->size, profile, sample_rate, nb_channels)) {
            ofs.write(reinterpret_cast<char *>(adts_header), ADTS_HEADER_LEN);
        } else {
            fprintf(stderr, "Failed to generate adts header\n");
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

static bool InnerEncodeAudioAAC(AVCodecContext *codec_ctx, AVFrame *frame, std::ifstream &ifs, std::ofstream &ofs) {
    if (!codec_ctx || !frame || !ifs || !ofs) {
        return false;
    }

    int error_code{};

    int bytes_per_sample = av_get_bytes_per_sample(codec_ctx->sample_fmt);
    if (bytes_per_sample <= 0) {
        fprintf(stderr, "Failed to get bytes per sample\n");
        return false;
    }

    // allocate AVBufferRef[] according to the codec parameters
    frame->format = codec_ctx->sample_fmt;
    frame->ch_layout = codec_ctx->ch_layout;
    frame->nb_samples = codec_ctx->frame_size;
    frame->sample_rate = codec_ctx->sample_rate;
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
    int nb_samples = frame->nb_samples;
    int nb_channels = frame->ch_layout.nb_channels;
    AVSampleFormat sample_fmt = codec_ctx->sample_fmt;
    int bytes_per_frame = bytes_per_sample * nb_channels * nb_samples;
    auto pcm_buffer_packed = std::make_unique<uint8_t[]>(bytes_per_frame);
    auto pcm_buffer_planar = std::make_unique<uint8_t[]>(bytes_per_frame);

    while (true) {
        // read pcm samples
        std::memset(pcm_buffer_packed.get(), 0, bytes_per_frame);
        if (!ifs.read(reinterpret_cast<char *>(pcm_buffer_packed.get()), bytes_per_frame)) {
            if (!ifs.eof()) {
                fprintf(stderr, "Failed to read input file: ifstream is broken\n");
                ret = false;
                break;
            }
        }
        int bytes_read = static_cast<int>(ifs.gcount());
        int samples_read = bytes_read / bytes_per_sample;
        int nb_samples_read = samples_read / nb_channels;
        if (nb_samples_read <= 0) {
            break;
        }

        // convert pcm sample format
        uint8_t *data = nullptr;
        if (av_sample_fmt_is_planar(sample_fmt)) {
            data = pcm_buffer_planar.get();
            std::memset(data, 0, bytes_per_frame);
            for (int i = 0; i < nb_channels; ++i) {
                for (int j = i; j < samples_read; j += nb_channels) {
                    std::memcpy(data, pcm_buffer_packed.get() + j * bytes_per_sample, bytes_per_sample);
                    data += bytes_per_sample;
                }
            }
            data = pcm_buffer_planar.get();
        } else {
            data = pcm_buffer_packed.get();
        }

        // initialize AVFrame
        if ((error_code = av_frame_make_writable(frame)) < 0) {
            fprintf(stderr, "Failed to make AVFrame writable: %s\n", ErrorToString(error_code));
            ret = false;
            break;
        }
        if ((error_code = av_samples_fill_arrays(frame->data, frame->linesize, data, nb_channels, nb_samples_read,
                                                 sample_fmt, 0)) < 0) {
            fprintf(stderr, "Failed to fill AVFrame data: %s\n", ErrorToString(error_code));
            ret = false;
            break;
        }
        pts += nb_samples;
        frame->pts = pts;

        // encode pcm to aac, write to file
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

void EncodeAudioAAC(int nb_channels, int sample_rate, AVSampleFormat sample_fmt, int64_t bit_rate,
                    const char *codec_name, const char *input_file, const char *output_file) {
    int error_code = 0;

    // find AVCodec, default aac encoder
    const AVCodec *codec = avcodec_find_encoder_by_name(codec_name);
    if (codec == nullptr) {
        fprintf(stderr, "AVCodec '%s' not found, use aac\n", codec_name);
        codec_name = "aac";
        codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
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

    // initialize AVCodecContext
    if (!SetSampleFormat(codec, codec_ctx, sample_fmt) || !SetSampleRate(codec, codec_ctx, sample_rate)) {
        avcodec_free_context(&codec_ctx);
        return;
    }
    av_channel_layout_default(&codec_ctx->ch_layout, nb_channels);
    codec_ctx->bit_rate = bit_rate;
    codec_ctx->profile = kDefaultProfile;

    // initialize AVCodec
    if ((error_code = avcodec_open2(codec_ctx, codec, nullptr)) < 0) {
        fprintf(stderr, "Failed to init AVCodecContext: %s\n", ErrorToString(error_code));
        avcodec_free_context(&codec_ctx);
        return;
    }
    printf("AVCodec '%s' initialized: sample_fmt='%s', sample_rate=%d, nb_channels=%d, bit_rate=%lld, frame_size=%d\n", codec_name, av_get_sample_fmt_name(sample_fmt), sample_rate, nb_channels, bit_rate, codec_ctx->frame_size);

    // allocate AVFrame
    AVFrame *frame = av_frame_alloc();
    if (frame == nullptr) {
        fprintf(stderr, "Failed to allocate AVFrame: av_frame_alloc()\n");
        avcodec_free_context(&codec_ctx);
        return;
    }

    printf("Start to encode audio\n");
    auto start = std::chrono::high_resolution_clock::now();
    InnerEncodeAudioAAC(codec_ctx, frame, ifs, ofs);
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    printf("End of encode audio, cost %lld ms\n", duration);

    av_frame_free(&frame);
    avcodec_free_context(&codec_ctx);
}

int main() {
    // ffmpeg -i yuv420p_640x360_25fps.mp4 -ar 48000 -ac 2 -f f32le 48k_f32le_2ch.pcm
    EncodeAudioAAC(2, 48000, AV_SAMPLE_FMT_FLTP, 128 * 1024, "aac", "../../../../48k_f32le_2ch.pcm", "../../../../48k_f32le_2ch.aac");
    // ffplay 48k_f32le_2ch.aac
    return 0;
}
