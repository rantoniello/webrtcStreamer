/**
 */

#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <queue>
#include <atomic>
#include <sys/time.h>
#include <rtc/rtc.hpp>

#include <iostream>
#include <fstream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

using namespace rtc;
using namespace std;
using namespace std::chrono_literals;
//#define DUMP_VIDEO2FILE

using json = nlohmann::json;

template <class T> weak_ptr<T> make_weak_ptr(shared_ptr<T> ptr) { return ptr; }

struct Frame {
    Frame(void *data, size_t size) :
            buf(size),
            data(reinterpret_cast<const void*>(this->buf.data())),
            size(size) {
        if (data != nullptr && size > 0)
            memcpy(this->buf.data(), data, size);
    }
    vector<std::byte> buf;
    const void *data;
    size_t size;
};
std::queue<Frame> framesFifo;
thread vstreamThread;

const string defaultIPAddress = "127.0.0.1";
const uint16_t defaultPort = 8000;
string ip_address = defaultIPAddress;
uint16_t port = defaultPort;

#if 0
void vstreamThr(shared_ptr<ClientTrackData> video)
{
    /* Init encoder */
    int width = 352, height = 288;
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (codec == nullptr)
        return;
    //std::unique_ptr<Bar, void(*)(Bar*)> ptr_; //FIXME!!
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
printf("**** %d\n", __LINE__); fflush(stdout); //FIXME!!
    /* Encoding loop */
    AVFrame *frame = av_frame_alloc();
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = width;
    frame->height = height;
    av_frame_get_buffer(frame, 0);
    int frameIndex = 0;
    int ret_code = 0;
    while (true) {
//        printf("**** %d\n", __LINE__); fflush(stdout); //FIXME!!
        // Generate a dummy frame (color pattern)
        av_frame_make_writable(frame);
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++)
                frame->data[0][y * frame->linesize[0] + x] = x + y + 
                frameIndex * 3;
        for (int y = 0; y < height / 2; y++)
            for (int x = 0; x < width / 2; x++) {
                frame->data[1][y * frame->linesize[1] + x] = 128;
                frame->data[2][y * frame->linesize[2] + x] = 64;
            }
        frameIndex++;
        struct timeval time;
        gettimeofday(&time, NULL);
        uint64_t sampleTime = uint64_t(time.tv_sec) * 1000 * 1000 + time.tv_usec;
        frame->pts = (sampleTime * 90) / 1000;
        frame->pkt_dts = (sampleTime * 90) / 1000;

        if (avcodec_send_frame(ctx, frame) == 0) {
            AVPacket *pkt = av_packet_alloc();
            while ((ret_code = avcodec_receive_packet(ctx, pkt)) >= 0) {                
                if (ret_code == AVERROR(EAGAIN) || ret_code == AVERROR_EOF)
                    break;
                if (pkt && pkt->data && pkt->size > 0) {
                    Frame frame(pkt->data, pkt->size);
                    video->track->sendFrame(
                        static_cast<const byte*>(frame.data), 
                        frame.size, 
                        std::chrono::duration<double, std::micro>(sampleTime));
                    //video->track->send(static_cast<const byte*>(frame.data), frame.size);
#ifdef DUMP_VIDEO2FILE
                    static bool start = false;
                    if (!start) {
                        unlink("/tmp/output.h264");
                        start = true;
                    }
                    std::ofstream outFile("/tmp/output.h264", std::ios::binary | std::ios::app);
                    if (outFile) {
                        outFile.write((const char*)pkt->data, pkt->size);
                        outFile.flush();
                        outFile.close();
                    }
#endif
                    //framesFifo.push(frame);
                }
                av_packet_unref(pkt);
            }
            av_packet_free(&pkt);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(33)); // ~30fps
    }

    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    return;
}

/// Incomming message handler for websocket
/// @param message Incommint message
/// @param config Configuration
/// @param ws Websocket
void wsOnMessage(json message, Configuration config, shared_ptr<WebSocket> ws) {
    auto it = message.find("id");
    if (it == message.end())
        return;
    string id = it->get<string>();

    it = message.find("type");
    if (it == message.end())
        return;
    string type = it->get<string>();

    if (type == "request") {
        clients.emplace(id, createPeerConnection(config, make_weak_ptr(ws), id));
    } else if (type == "answer") {
        if (auto jt = clients.find(id); jt != clients.end()) {
            auto pc = jt->second->peerConnection;
            auto sdp = message["sdp"].get<string>();
            auto description = Description(sdp, type);
            pc->setRemoteDescription(description);
        }
    }
}
#endif

/// Creates peer connection and client representation
/// @param config Configuration
/// @param wws Websocket for signaling
/// @param id Client ID
/// @returns Client
// Create and setup a PeerConnection
class WebRTCAVStreamer {
public:
    WebRTCAVStreamer();
    ~WebRTCAVStreamer();
    void addRemotePeer(string id);
    void getRemotePeerDescription(string description);
private:
    ///
    shared_mutex _api_mutex;
    ///
    Configuration _conf;
    /// Peers register
    unordered_map<string, shared_ptr<PeerConnection>> _peersMap;
    /// Signaling web socket
    shared_ptr<WebSocket> _signalingWS;
    /// Signaling thread
    thread _signalingThread;
    ///
    atomic_bool _do_term_signaling;
};

WebRTCAVStreamer::WebRTCAVStreamer() :
        _api_mutex(),
        _conf(),
        _peersMap{},
        _signalingWS(make_shared<WebSocket>()),
        _do_term_signaling(false)
{
    // Configuration: set STUN sevrer
    string stunServer = "stun:stun.l.google.com:19302";
    cout << "STUN server is " << stunServer << endl;
    _conf.iceServers.emplace_back(stunServer);

    // Configuration: disable auto-negotiation. If set to true, the user is
    // responsible for calling 'rtcSetLocalDescription' after creating a data
    // channel and after setting the remote description
    _conf.disableAutoNegotiation = true;

    // Initialize signaling web socket
    _signalingWS->onOpen([]() {
        cout << "WebSocket connected, signaling ready" << endl;
        //TODO: FIFO push {"signaling":"connected"}
    });
    _signalingWS->onClosed([&]() {
        cout << "WebSocket closed" << endl;
        _do_term_signaling = true;
        //TODO: FIFO push {"signaling":"closed"}
    });
    _signalingWS->onError([](const string &error) {
        cout << "WebSocket failed: " << error << endl; 
    });
    _signalingWS->onMessage([&](variant<binary, string> data) {
        if (!holds_alternative<string>(data))
            return;
        json message = json::parse(get<string>(data));
        //TODO: FIFO push {"signaling":"input message","body":<message>}
        //wsOnMessage(message, config, ws); //FIXME!!: move to signaling thread
    });
    string localId = "server";
    cout << "The local ID is: " << localId << endl;
    const string url = "ws://" + ip_address + ":" + to_string(port) + "/" + localId; //FIXME!!: add in constructor
    cout << "URL is " << url << endl;
    _signalingWS->open(url);

    //TODO: launch signaling thread
    _signalingThread = thread([&] {
            std::cout << "Launching signaling thread..." <<  "\n";

            while (!_do_term_signaling) {
                std::cout << "Signaling thread..." <<  "\n";
                this_thread::sleep_for(std::chrono::seconds(1)); //FIXME!!

                //TODO read FIFO messages and execute!
            }
    });
}

WebRTCAVStreamer::~WebRTCAVStreamer()
{
    _signalingWS->close();

    //TODO: wait join signaling thread (thus confir it recieves close!! and unlock)
    //TODO: join signaling thread (after stoping web socket)
    _signalingThread.join();
}

void WebRTCAVStreamer::addRemotePeer(string peerId)
{
    unique_lock lock(_api_mutex);

    // Allocate peer connection
    auto pc = make_shared<PeerConnection>(_conf);

    // Callbacks...
    pc->onStateChange([peerId](PeerConnection::State state) {
        cout << "State: " << state << endl;
        if (state == PeerConnection::State::Disconnected ||
            state == PeerConnection::State::Failed ||
            state == PeerConnection::State::Closed) {
            // remove disconnected client //TODO
        }
    });
    pc->onGatheringStateChange([wpc = make_weak_ptr(pc), peerId]
            (PeerConnection::GatheringState state) {
        cout << "Gathering State: " << state << endl;
        if (state == PeerConnection::GatheringState::Complete) {
            if(auto pc = wpc.lock()) {
                auto description = pc->localDescription();
                json message = {
                    {"id", peerId},
                    {"type", description->typeString()},
                    {"sdp", string(description.value())}
                };
                // Gathering complete, send answer
                //if (auto ws = wws.lock()) {
                //    ws->send(message.dump());
                //}
                //TODO: FIFO push {"signaling":"output message","body":<message>}
            }
        }
    });

    // Add video track to peer connection
    const uint8_t payloadType = 102;
    const uint32_t ssrc = 1;
    const string cname = "video-stream";
    const string msid = "stream1";
    auto video = Description::Video(cname);
    video.addH264Codec(payloadType);
    video.addSSRC(ssrc, cname, msid, cname);
    auto track = pc->addTrack(video);
    // create RTP configuration
    auto rtpConfig = make_shared<RtpPacketizationConfig>(ssrc, cname, 
        payloadType, H264RtpPacketizer::ClockRate);
    // create packetizer
    auto packetizer = make_shared<H264RtpPacketizer>(
        NalUnit::Separator::StartSequence, rtpConfig);
    // add RTCP SR handler
    auto srReporter = make_shared<RtcpSrReporter>(rtpConfig);
    packetizer->addToChain(srReporter);
    // add RTCP NACK handler
    auto nackResponder = make_shared<RtcpNackResponder>();
    packetizer->addToChain(nackResponder);
    // set handler
    track->setMediaHandler(packetizer);
    const function<void (void)> onOpen = [peerId]() {
        cout << "Video from " << peerId << " opened" << endl;
        //TODO: FIFO push {"signaling":"video track opened","peerId":<id>}
        //vstreamThread = thread(vstreamThr, video); //FIXME!!
    };
    track->onOpen(onOpen);

    auto dc = pc->createDataChannel("ping-pong");
    dc->onOpen([peerId, wdc = make_weak_ptr(dc)]() {
        //TODO: FIFO push {"signaling":"data channel opened","peerId":<id>}
        //if (auto dc = wdc.lock()) {dc->send("Ping");} //FIXME!!: action to do
    });
    dc->onMessage(nullptr, [peerId, wdc = make_weak_ptr(dc)](string msg) {
        cout << "Message from " << peerId << " received: " << msg << endl;
        //if (auto dc = wdc.lock()) {dc->send("Ping");} //FIXME!!: action to do
    });

    // Set local description
    pc->setLocalDescription();

    // Finally register peer connection in peers map.
    this->_peersMap.emplace(peerId, make_weak_ptr(pc));
}

void WebRTCAVStreamer::getRemotePeerDescription(string description)
{
    unique_lock lock(_api_mutex);
}

int main(int argc, char **argv) try {
    int c = 0;

    InitLogger(LogLevel::Debug);

    WebRTCAVStreamer streamer;

    string localId = "server";
    cout << "The local ID is: " << localId << endl;

    while (true) {
        string id;
        cout << "Enter to exit" << endl;
        cin >> id;
        cin.ignore();
        cout << "exiting" << endl;
        break;
    }

    cout << "Cleaning up..." << endl;
    return 0;

} catch (const std::exception &e) {
    std::cout << "Error: " << e.what() << std::endl;
    return -1;
}

