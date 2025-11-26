#include <rtc/rtc.hpp>
#include <libwebsockets.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <thread>
#include <string>
#include <vector>
#include <chrono>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

using json = nlohmann::json;

static struct lws_context *context;
static struct lws *browser_wsi = nullptr;
static rtc::PeerConnection *pc = nullptr;
static rtc::Track *track = nullptr;

// Forward declarations
void onLocalDescription(rtc::Description desc);
void onLocalCandidate(rtc::Candidate cand);
void websocketLoop();
void initFFmpegEncoder(int width, int height);

// Send message to browser via WebSocket
void send_ws(const std::string &message) {
    if (browser_wsi) {
        unsigned char buf[LWS_PRE + 4096];
        unsigned char *p = &buf[LWS_PRE];
        size_t n = message.size();
        memcpy(p, message.c_str(), n);
        lws_write(browser_wsi, p, n, LWS_WRITE_TEXT);
    }
}

// Handle incoming signaling message
void handleMessage(const std::string &msg) {
    printf("######################## handleMessage: reason num. %s\n", msg.c_str());
    try {
        json data = json::parse(msg);

        if (data["type"] == "offer") {
            std::string sdp = data["sdp"];

            // Replace escaped newlines with actual CRLF
            size_t pos = 0;
            while ((pos = sdp.find("\\r\\n", pos)) != std::string::npos) {
                sdp.replace(pos, 4, "\r\n");
            }

            std::cout << "Received SDP:\n" << sdp << std::endl;

            pc->setRemoteDescription(rtc::Description(sdp, "offer"));
            pc->createAnswer();
        } else if (data["type"] == "candidate") {
            std::string candidate = data["candidate"];
            std::string sdpMid = data.value("sdpMid", "");
            pc->addRemoteCandidate(rtc::Candidate(candidate, sdpMid));
        }
    } catch (const std::exception &e) {
        std::cerr << "JSON parse error: " << e.what() << std::endl;
    }
}

// WebSocket callback
static int callback_ws(struct lws *wsi, enum lws_callback_reasons reason,
                       void *user, void *in, size_t len) {
    printf("######################## callback_ws: reason num. %d\n", reason);
    switch (reason) {
        case LWS_CALLBACK_ESTABLISHED:
            browser_wsi = wsi;
            std::cout << "Browser connected\n";
            break;

        case LWS_CALLBACK_RECEIVE: {
            std::string msg((char *)in, len);
            handleMessage(msg);
            break;
        }
        default:
            break;
    }
    return 0;
}

// Protocols
static struct lws_protocols protocols[] = {
    {"ws", callback_ws, 0, 0},
    {NULL, NULL, 0, 0}
};

// Event handlers without lambdas
void onLocalDescription(rtc::Description desc) {
     printf("onLocalDescription ==============================================================\n");
    json msg = {{"type", desc.typeString()}, {"sdp", std::string(desc)}};
    send_ws(msg.dump());
}

void onLocalCandidate(rtc::Candidate cand) {
    //json msg = {{"type", "candidate"}, {"candidate", cand.candidate()}};
         printf("onLocalCandidate ==============================================================\n");
json msg = {
        {"type", "candidate"},
        {"candidate", cand.candidate()}
    };
    send_ws(msg.dump());
}

void onStateChange(rtc::PeerConnection::State state) {
    std::cout << "PeerConnection state: " << (int)state << std::endl;
    printf("==============================================================\n");
    fflush(stdout);
}

// Dedicated thread function for libwebsockets loop
void websocketLoop() {
    while (true) {
        lws_service(context, 50);
    }
}

// FFmpeg encoder setup
AVCodecContext* initEncoder(int width, int height) {
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) throw std::runtime_error("H.264 codec not found");

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
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

    if (avcodec_open2(ctx, codec, nullptr) < 0)
        throw std::runtime_error("Could not open codec");

    return ctx;
}

int main() {
    rtc::InitLogger(rtc::LogLevel::Info);

    pc = new rtc::PeerConnection();
    //rtc::Configuration config;
    //config.iceServers.emplace_back("stun:stun.l.google.com:19302");
    //pc = new rtc::PeerConnection(config);

    pc->onLocalDescription(onLocalDescription);
    pc->onLocalCandidate(onLocalCandidate);
    pc->onStateChange(onStateChange);
    pc->onGatheringStateChange([pc](rtc::PeerConnection::GatheringState state) {
			std::cout << "############## Gathering State: " << state << std::endl;
			if (state == rtc::PeerConnection::GatheringState::Complete) {
				auto description = pc->localDescription();
				json message = {{"type", description->typeString()},
				                {"sdp", std::string(description.value())}};
				std::cout << message << std::endl;
			}
		});

    const rtc::SSRC ssrc = 42;
    rtc::Description::Video media("video", rtc::Description::Direction::SendOnly);
    media.addH264Codec(96); // Must match the payload type of the external h264 RTP stream
    media.addSSRC(ssrc, "video-send");
    auto track = pc->addTrack(media);
    //pc->setLocalDescription();

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = 8080;
    info.protocols = protocols;

    context = lws_create_context(&info);
    if (!context) {
        std::cerr << "Failed to create LWS context\n";
        return 1;
    }

    
   struct lws_client_connect_info ccinfo = {0};
    ccinfo.context = context;
    ccinfo.address = "localhost";
    ccinfo.port = 8080;
    ccinfo.path = "/";
    ccinfo.host = lws_canonical_hostname(context);
    ccinfo.origin = "origin";
    ccinfo.protocol = "signaling";

    browser_wsi = lws_client_connect_via_info(&ccinfo);
    if (!browser_wsi) {
        std::cerr << "Failed to connect to signaling server." << std::endl;
        return -1;
    }

    std::thread wsThread(websocketLoop);
    wsThread.detach();

   // FFmpeg encoder
    int width = 640, height = 480;
    AVCodecContext *encoderCtx = initEncoder(width, height);

    AVFrame *frame = av_frame_alloc();
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = width;
    frame->height = height;
    av_frame_get_buffer(frame, 0);

    int frameIndex = 0;

    while (true) {
        //if (track->isOpen()) {
            // Generate a dummy frame (color pattern)
            av_frame_make_writable(frame);
            for (int y = 0; y < height; y++)
                for (int x = 0; x < width; x++)
                    frame->data[0][y * frame->linesize[0] + x] = x + y + frameIndex * 3;
            for (int y = 0; y < height / 2; y++)
                for (int x = 0; x < width / 2; x++) {
                    frame->data[1][y * frame->linesize[1] + x] = 128;
                    frame->data[2][y * frame->linesize[2] + x] = 64;
                }

            frame->pts = frameIndex++;
            //encodeAndSendFrame(encoderCtx, frame);
            if (avcodec_send_frame(encoderCtx, frame) == 0) {
                AVPacket *pkt = av_packet_alloc();
                while (avcodec_receive_packet(encoderCtx, pkt) == 0) {
                    //printf("**** %d\n", __LINE__); fflush(stdout);
                    if (pc->state() == rtc::PeerConnection::State::Connected)
                    {
                        printf("**** %d\n", __LINE__); fflush(stdout);
                        if (pkt && pkt->data && pkt->size > 0) {
                            std::vector<std::byte> frame(pkt->size);
                            memcpy(frame.data(), pkt->data, pkt->size);
                            track->send(frame);
                        }
                    }
                    //track->send(reinterpret_cast<const std::byte *>(pkt->data), pkt->size);
                    av_packet_unref(pkt);
                }
                av_packet_free(&pkt);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(33)); // ~30fps
        //}
    }

    av_frame_free(&frame);
    avcodec_free_context(&encoderCtx);
    return 0;
}
