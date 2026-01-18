extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <cmath>

int dump_format(const char *input_file) {
    int ret = 0;
    AVFormatContext *fmt_ctx = nullptr;
    if ((ret = avformat_open_input(&fmt_ctx, input_file, nullptr, nullptr)) < 0) {
        return ret;
    }
    if ((ret = avformat_find_stream_info(fmt_ctx, nullptr)) < 0) {
        avformat_close_input(&fmt_ctx);
        return ret;
    }
    av_dump_format(fmt_ctx, 0, input_file, 0);
    avformat_close_input(&fmt_ctx);
    return ret;
}

int demultiplex(const char *filename) {
    int ret = 0;
    int audio_idx = 0, video_idx = 0;

    AVFormatContext *fmt_ctx = nullptr;
    if ((ret = avformat_open_input(&fmt_ctx, filename, nullptr, nullptr)) < 0) {
        return ret;
    }
    if ((ret = avformat_find_stream_info(fmt_ctx, nullptr)) < 0) {
        avformat_close_input(&fmt_ctx);
        return ret;
    }

    // dump by api
    av_dump_format(fmt_ctx, 0, filename, 0);

    // dump by user
    printf("Input #0, %s, from \'%s\':\n", fmt_ctx->iformat->name, fmt_ctx->url);
    for (int i = 0; i < fmt_ctx->nb_streams; i++) {
        AVStream *stream = fmt_ctx->streams[i];
        AVCodecParameters *param = stream->codecpar;

        // stream->duration is in stream->time_base units
        const double total_seconds = static_cast<double>(stream->duration) * av_q2d(stream->time_base);
        printf("%lf\n", av_q2d(stream->time_base));

        const int hour_part = static_cast<int>(total_seconds / 3600);
        const int minute_part = static_cast<int>((total_seconds - hour_part * 3600) / 60);
        const int second_part = static_cast<int>(total_seconds - hour_part * 3600 - minute_part * 60);
        const int ms_part = static_cast<int>(std::fmod(total_seconds, 1.0) * 1000);

        const int bit_rate_kb = static_cast<int>(param->bit_rate / 1000);
        if (param->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_idx = i;
            printf("Stream #0:%d Video: ", video_idx);
            const int width = param->width, height = param->height;
            const int frame_rate = static_cast<int>(av_q2d(stream->avg_frame_rate));
            if (param->codec_id == AV_CODEC_ID_H264) {
                printf("h264, %dx%d, %d kb/s, %d fps, ", width, height, bit_rate_kb, frame_rate);
            } else {
                printf("not h264, %dx%d, %d kb/s, %d fps, ", width, height, bit_rate_kb, frame_rate);
            }
            printf("%d:%d:%d:%d\n", hour_part, minute_part, second_part, ms_part);
        } else if (param->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_idx = i;
            printf("Stream #0:%d Audio: ", audio_idx);
            const int sample_rate = param->sample_rate;
            const int channels = param->ch_layout.nb_channels;
            if (param->codec_id == AV_CODEC_ID_AAC) {
                printf("aac, %d Hz, %d channels, %d kb/s, ", sample_rate, channels, bit_rate_kb);
            } else {
                printf("not aac, %d Hz, %d channels, %d kb/s, ", sample_rate, channels, bit_rate_kb);
            }
            printf("%d:%d:%d:%d\n", hour_part, minute_part, second_part, ms_part);
        } else {
            printf("Stream #0:%d not video or audio\n", i);
        }
    }

    AVPacket *pkt = nullptr;
    if ((pkt = av_packet_alloc()) == nullptr) {
        fprintf(stderr, "Could not allocate AVPacket\n");
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    while (true) {
        if ((ret = av_read_frame(fmt_ctx, pkt))) {
            if (ret != AVERROR_EOF) {
            }
            break;
        }

        if (pkt->stream_index == audio_idx) {
            const AVRational time_base = fmt_ctx->streams[audio_idx]->time_base;
            const double second_ts = static_cast<double>(pkt->pts) * av_q2d(time_base);
            const double second_duration = static_cast<double>(pkt->duration) * av_q2d(time_base);
            printf("\taudio frame: pts=%lld, dts=%lld, size=%d, ", pkt->pts, pkt->dts, pkt->size);
            printf("pos=%lld, time=%lf, duration=%lf\n", pkt->pos, second_ts, second_duration);
        } else if (pkt->stream_index == video_idx) {
            const AVRational time_base = fmt_ctx->streams[video_idx]->time_base;
            const double second_ts = static_cast<double>(pkt->pts) * av_q2d(time_base);
            const double second_duration = static_cast<double>(pkt->duration) * av_q2d(time_base);
            printf("\tvideo frame: pts=%lld, dts=%lld, size=%d, ", pkt->pts, pkt->dts, pkt->size);
            printf("pos=%lld, time=%lf, duration=%lf\n", pkt->pos, second_ts, second_duration);
        } else {
            printf("Unknown stream_index: %d\n", pkt->stream_index);
        }

        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    avformat_close_input(&fmt_ctx);
    return 0;
}

void test_memory() {
    AVFrame *frame1 = av_frame_alloc(); // allocate AVFrame
    frame1->format = AV_PIX_FMT_YUV420P; // planar YUV 4:2:0
    frame1->width = 640;
    frame1->height = 480;
    av_frame_get_buffer(frame1, 0); // allocate AVBufferRef[] according to the format
    fprintf(stderr, "frame1->linesize[0]: %d\n", frame1->linesize[0]); // Y: 640
    fprintf(stderr, "frame1->linesize[1]: %d\n", frame1->linesize[1]); // U: 320=640/2
    fprintf(stderr, "frame1->linesize[2]: %d\n", frame1->linesize[2]); // V: 320=640/2

    AVFrame *frame2 = av_frame_alloc();
    frame2->format = AV_PIX_FMT_NV21; // planar Y, packed UV, 4:2:0
    frame2->width = 640;
    frame2->height = 480;
    av_frame_get_buffer(frame2, 0);
    fprintf(stderr, "frame2->linesize[0]: %d\n", frame2->linesize[0]); // Y: 640
    fprintf(stderr, "frame2->linesize[1]: %d\n", frame2->linesize[1]); // UV: 640=320+320

    av_frame_unref(frame1); // free AVBufferRef[]
    av_frame_unref(frame2); // free AVBufferRef[]
    av_frame_free(&frame1); // free AVFrame
    av_frame_free(&frame2); // free AVFrame
}

int main() {
    const char *input_file = "../../../../yuv420p_640x360_25fps.mp4";
    // return dump_format(input_file);
    return demultiplex(input_file);
}
