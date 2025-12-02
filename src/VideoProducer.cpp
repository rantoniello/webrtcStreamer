/**
 */

#include "VideoProducer.h"
#include <unistd.h>
#include <iostream>
#include <fstream>
#include <chrono>
#include <mutex>
#include <optional>
#include <sys/time.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

VideoProducer::VideoProducer(
        function<void(const uint8_t *data, size_t size)> onSample,
        string inputUrl) :
    sampleHandler(onSample),
    inputUrl(inputUrl),
    _do_term(false)
{
    _videoThread = thread([&] {dummyStreamThr(); });
}

VideoProducer::~VideoProducer()
{
    _do_term = true;
    _videoThread.join();
}

static void dump2file(const uint8_t *data, size_t size)
{
    static bool start = false;
    if (!start) {
        unlink("/tmp/output.h264");
        start = true;
    }
    std::ofstream outFile("/tmp/output.h264", std::ios::binary | std::ios::app);
    if (outFile) {
        outFile.write((const char*)data, size);
        outFile.flush();
        outFile.close();
    }
}

void VideoProducer::dummyStreamThr()
{
    const int width = 352, height = 288;
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (codec == nullptr)
        return;

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (ctx == nullptr)
        return;

    ctx->bit_rate = 400000;
    ctx->width = width;
    ctx->height = height;
    ctx->time_base = {1, 30};
    ctx->framerate = {30, 1};
    ctx->gop_size = 10;
    ctx->max_b_frames = 1;
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
    if (avcodec_open2(ctx, codec, nullptr) < 0)  {
        avcodec_free_context(&ctx);
        return;
    }

    AVFrame *frame = av_frame_alloc();
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = width;
    frame->height = height;
    av_frame_get_buffer(frame, 0);
    int frameIndex = 0;
    int ret_code = 0;
    bool dumpVideo = false;

    /* Encoding loop */
    while (!_do_term) {
        // Generate a dummy frame (color pattern)
        av_frame_make_writable(frame);
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++)
                frame->data[0][y * frame->linesize[0] + x] =
                    x + y + frameIndex * 3;
        for (int y = 0; y < height / 2; y++) {
            for (int x = 0; x < width / 2; x++) {
                frame->data[1][y * frame->linesize[1] + x] = 128;
                frame->data[2][y * frame->linesize[2] + x] = 64;
            }
        }
        frameIndex++;

        struct timeval time;
        gettimeofday(&time, NULL);
        uint64_t sampleTime_usecs = uint64_t(time.tv_sec) * 1000 * 1000 +
                time.tv_usec;
        frame->pts = (sampleTime_usecs * 90) / 1000;

        if (avcodec_send_frame(ctx, frame) == 0) {
            AVPacket *pkt = av_packet_alloc();
            while ((ret_code = avcodec_receive_packet(ctx, pkt)) >= 0) {
                if (ret_code == AVERROR(EAGAIN) || ret_code == AVERROR_EOF)
                    break;
                if (pkt && pkt->data && pkt->size > 0) {
                    if (dumpVideo)
                        dump2file(pkt->data, pkt->size);
                    sampleHandler(pkt->data, pkt->size);
                }
                av_packet_unref(pkt);
            }
            av_packet_free(&pkt);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(33)); // ~30fps
    }

    av_frame_free(&frame);
    avcodec_free_context(&ctx);
}
