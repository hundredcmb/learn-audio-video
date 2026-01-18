extern "C" {
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}

#include <fstream>

thread_local static char error_buffer[AV_ERROR_MAX_STRING_SIZE] = {}; // store FFmpeg error string

static char *ErrorToString(const int error_code) {
    std::memset(error_buffer, 0, AV_ERROR_MAX_STRING_SIZE);
    return av_make_error_string(error_buffer, AV_ERROR_MAX_STRING_SIZE, error_code);
}

int ResampleAudio(const char *input_file, const char *output_file) {
    int error_code = 0;
    SwrContext *swr_ctx = nullptr;
    AVChannelLayout in_ch_layout = AV_CHANNEL_LAYOUT_STEREO;
    AVSampleFormat in_sample_fmt = AV_SAMPLE_FMT_FLT;
    int in_sample_rate = 48000;
    AVChannelLayout out_ch_layout = AV_CHANNEL_LAYOUT_STEREO;
    AVSampleFormat out_sample_fmt = AV_SAMPLE_FMT_S16;
    int out_sample_rate = 44100;

    // open input_file and output_file
    std::ifstream ifs(input_file, std::ios::in | std::ios::binary);
    if (!ifs.is_open()) {
        fprintf(stderr, "Failed to open input file: %s\n", input_file);
        return -1;
    }
    std::ofstream ofs(output_file, std::ios::out | std::ios::binary);
    if (!ofs.is_open()) {
        fprintf(stderr, "Failed to open output file: %s\n", output_file);
        return -1;
    }

    // alloc swr_ctx
    error_code = swr_alloc_set_opts2(&swr_ctx, &out_ch_layout, out_sample_fmt, out_sample_rate, &in_ch_layout,
                                     in_sample_fmt, in_sample_rate, 0, nullptr);
    if (error_code < 0 || swr_ctx == nullptr) {
        fprintf(stderr, "Failed to alloc swr_ctx: %s\n", ErrorToString(error_code));
    }

    // init swr
    if ((error_code = swr_init(swr_ctx)) < 0) {
        fprintf(stderr, "Failed to init swr_ctx: %s\n", ErrorToString(error_code));
        swr_free(&swr_ctx);
        return error_code;
    }

    // allocate input buffer and output buffer
    const std::size_t input_buffer_size = av_get_bytes_per_sample(in_sample_fmt) * 1024 * 2;
    auto input_buffer = std::make_unique<uint8_t[]>(input_buffer_size);
    std::memset(input_buffer.get(), 0, input_buffer_size);
    const std::size_t output_buffer_size = av_get_bytes_per_sample(out_sample_fmt) * 1024 * 2;
    auto output_buffer = std::make_unique<uint8_t[]>(output_buffer_size);
    std::memset(output_buffer.get(), 0, output_buffer_size);

    size_t data_size{};
    while (true) {
        if (!ifs.read(reinterpret_cast<char *>(input_buffer.get()), static_cast<std::streamsize>(input_buffer_size))) {
            if (!ifs.eof()) {
                fprintf(stderr, "Failed to read input file: %s\n", input_file);
                break;
            }
            fprintf(stderr, "End of ifstream: %s\n", input_file);
        }
        data_size = ifs.gcount();
        if (data_size == 0) {
            break;
        }

        // init input_frame
        AVFrame *input_frame = av_frame_alloc();
        if (input_frame == nullptr) {
            fprintf(stderr, "Failed to alloc input_frame: av_frame_alloc()\n");
            break;
        }
        input_frame->format = in_sample_fmt;
        input_frame->ch_layout = in_ch_layout;
        input_frame->sample_rate = in_sample_rate;
        input_frame->nb_samples = static_cast<int>(data_size) / av_get_bytes_per_sample(in_sample_fmt) / input_frame->
                                  ch_layout.nb_channels;
        if ((error_code = av_frame_get_buffer(input_frame, 0)) < 0) {
            fprintf(stderr, "Failed to av_frame_get_buffer(): %s\n", ErrorToString(error_code));
            av_frame_free(&input_frame);
            break;
        }
        std::memcpy(input_frame->data[0], input_buffer.get(), data_size);

        // init output_frame
        AVFrame *output_frame = av_frame_alloc();
        if (output_frame == nullptr) {
            fprintf(stderr, "Failed to alloc output_frame: av_frame_alloc()\n");
            av_frame_free(&input_frame);
            break;
        }
        output_frame->format = out_sample_fmt;
        output_frame->ch_layout = out_ch_layout;
        output_frame->sample_rate = out_sample_rate;
        output_frame->nb_samples = static_cast<int>(av_rescale_rnd(input_frame->nb_samples, out_sample_rate,
                                                                   in_sample_rate, AV_ROUND_UP));

        printf("%d -> %d\n", input_frame->nb_samples, output_frame->nb_samples);

        // convert
        error_code = swr_convert_frame(swr_ctx, output_frame, input_frame);
        if (error_code == 0) {
            if (!ofs.write(reinterpret_cast<char *>(output_frame->data[0]),
                      output_frame->nb_samples * av_get_bytes_per_sample(out_sample_fmt) * output_frame->ch_layout.
                      nb_channels)) {
                fprintf(stderr, "Failed to write pcm file, ofstream is broken\n");
            }
        } else {
            fprintf(stderr, "Failed to convert frame: %s\n", ErrorToString(error_code));
        }

        av_frame_free(&output_frame);
        av_frame_free(&input_frame);
    }

    swr_free(&swr_ctx);
    return 0;
}

int main() {
    // ffplay -ar 48000 -ac 2 -f f32le 48k_f32le_2ch.pcm
    ResampleAudio("../../../../48k_f32le_2ch.pcm", "../../../../44.1k_s16le_2ch.pcm");
    // ffplay -ar 44100 -ac 2 -f s16le 44.1k_s16le_2ch.pcm
    return 0;
}
