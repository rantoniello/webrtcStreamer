/**
 */

#include "WebRTCStreamer.h"

#include <sys/time.h>
#include <unistd.h>
#include <iostream>
#include <fstream>
#include <nlohmann/json.hpp>

using namespace rtc;
using namespace std::chrono_literals;
using json = nlohmann::json;

template <class T> weak_ptr<T> make_weak_ptr(shared_ptr<T> ptr) { return ptr; }

static optional<nlohmann::json> safeParse(const string& s) {
    try {
        return json::parse(s);
    } catch (const json::exception& e) {
        cout << "JSON parsing failed: " << e.what() << "\n";
        return nullopt;
    }
}

WebRTCStreamer::Client::Client(const string& id, const string& stunServer = ""):
    _id(id),
    _signalingQueueIn(32),
    _signalingQueueOut(32),
    _stunServer(stunServer),
    _do_term(false)
{
    // Allocate peer connection
    Configuration conf;
    if (_stunServer.length()) {
        cout << "STUN server is " << stunServer << endl;
        conf.iceServers.emplace_back(stunServer);
    }
    // - Disable auto-negotiation. If set to true, the user is
    // responsible for calling 'rtcSetLocalDescription' after creating a data
    // channel and after setting the remote description
    conf.disableAutoNegotiation = true;
    _peerConnection = make_shared<PeerConnection>(conf);

    // Callbacks...
    _peerConnection->onStateChange([this](PeerConnection::State state) {
        cout << "State: " << state << endl;
        if (state == PeerConnection::State::Disconnected ||
            state == PeerConnection::State::Failed ||
            state == PeerConnection::State::Closed) {
            // request to delete this client
            json message = {
                {"id", this->_id},
                {"io_mode", "output"},
                {"type", "disconnect"}
            };
            this->_signalingQueueOut.push(message.dump());
        }
    });
    _peerConnection->onGatheringStateChange(
            [this, wpc = make_weak_ptr(_peerConnection)]
            (PeerConnection::GatheringState state) {
        cout << "Gathering State: " << state << endl;
        if (state == PeerConnection::GatheringState::Complete) {
            if(auto pc = wpc.lock()) {
                auto description = pc->localDescription();
                json message = {
                    {"id", this->_id},
                    {"io_mode", "output"},
                    {"type", description->typeString()},
                    {"sdp", string(description.value())}
                };
                // Gathering complete, send answer
                this->_signalingQueueOut.push(message.dump());
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
    videoTrack = _peerConnection->addTrack(video);
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
    videoTrack->setMediaHandler(packetizer);
    const function<void (void)> onOpen = [this]() {
        cout << "Video from " << this->_id << " opened" << endl;
    };
    videoTrack->onOpen(onOpen);

    // Add data track
    dataChannel = _peerConnection->createDataChannel("ping-pong");
    dataChannel->onOpen([this, wdc = make_weak_ptr(dataChannel)]() {
        if (auto dc = wdc.lock()) {dc->send("Ping");}
    });
    dataChannel->onMessage(nullptr, [this, wdc = make_weak_ptr(dataChannel)]
            (string msg) {
        if (auto dc = wdc.lock()) {dc->send("Ping");}
    });

    // Set local description
    _peerConnection->setLocalDescription();

    // Launch signaling thread
    _signalingThread = thread([&] {
        while(!_do_term) {
            cout << "Signaling thread polling..." << "\n";
            optional<string> opt_message = _signalingQueueIn.pop();
            if (opt_message == nullopt)
                continue;
            auto description = Description(opt_message.value(), "answer");
            _peerConnection->setRemoteDescription(description);
        }
    });
}

WebRTCStreamer::Client::~Client()
{
    _do_term = true;
    _signalingQueueIn.stop();
    _signalingQueueOut.stop();
    _peerConnection->close();
    _signalingThread.join();
    cout << "Client '" << _id << "' released" << endl;
}

void WebRTCStreamer::Client::signalingSet(string& message)
{
    _signalingQueueIn.push(message);
}

optional<string> WebRTCStreamer::Client::signalingGet(
    chrono::milliseconds timeout)
{
    return _signalingQueueOut.pop_for(timeout);
}

void WebRTCStreamer::Client::sendFrame(const byte *data, size_t size,
        rtc::FrameInfo info)
{
    unique_lock lock(_publicTrackMutex);
    // NOTE: for this POC client manages only one media track (video source)
    if (videoTrack->isOpen()) {
        try {
            videoTrack->sendFrame(data, size, info);
        } catch (const json::exception& e) {
        }
    }
}

WebRTCStreamer::WebRTCStreamer(const string& signalingServer) :
        _publicApiMutex(),
        _do_term(false),
        _clientsMap{},
        _signalingServer(signalingServer),
        _signalingWS(make_shared<WebSocket>()),
        _signalingQueue(256),
        _framesQueueIn(256)
{
    // Launch i/o signaling thread
    _signalingThread = thread([&] {this->_signalingThr(); });

    // Launch multimedia processing thread
    _multimediaThread = thread([&] {this->_multimediaThr(); });
}

WebRTCStreamer::~WebRTCStreamer()
{
    _do_term = true;

    _signalingQueue.stop();
    _signalingWS->close();
    _signalingThread.join();

    _framesQueueIn.stop();
    _multimediaThread.join();
    cout << "WebRTCStreamer released" << _signalingServer << endl;
}

void WebRTCStreamer::pushData(void *const data, size_t size)
{
    unique_lock lock(_publicApiMutex);
    _pushData(data, size);
}

void WebRTCStreamer::_signalingThr()
{
    cout << "Launching signaling thread (input)..." << "\n";
    cout << "URL is " << _signalingServer << endl;

    // Initialize signaling web socket callbacks
    _signalingWS->onOpen([]() {
        cout << "WebSocket connected, signaling ready" << endl;
    });
    _signalingWS->onClosed([&]() {
        cout << "WebSocket closed" << endl;
        if (!_do_term) {
            cout << "WebSocket unexpectedly closed, try to reopen..." << endl;
            // sleep: simple throttling to avoid busy "closed" loops
            this_thread::sleep_for(chrono::milliseconds(1));
            // Open web-socket
            _signalingWS->open(_signalingServer);
        }
    });
    _signalingWS->onError([](const string &error) {
        cout << "WebSocket failed: " << error << endl;
    });
    _signalingWS->onMessage([&](variant<binary, string> data) {
        if (!holds_alternative<string>(data))
            return;
        string message = get<string>(data);
        cout << "Received message '" << message << "'\n";
        optional<nlohmann::json> opt_json_msg = safeParse(message);
        if (opt_json_msg == nullopt)
            return;
        json json_msg = opt_json_msg.value();
        json_msg["io_mode"] = "input";
        _signalingQueue.push(json_msg.dump());
    });

    thread signalingThreadOut = thread([&] {
        while(!_do_term) {
            _clientsMap.traverse([&](string id, shared_ptr<Client> client){
                optional<string> opt_message = client-> signalingGet(
                        (chrono::milliseconds)100);
                if (opt_message == nullopt)
                    return;
                _signalingQueue.push(opt_message.value());
            });
        }
    });

    // Open web-socket
    _signalingWS->open(_signalingServer);

    auto inputMessageHandler = [&](const string& id, const string& type,
            json json_msg)
    {
        cout << "Signaling input message: '" << json_msg.dump() << "'\n";

        if (type == "request") {
            cout << "Instantiating new client peer..." << "\n";
            _clientsMap.emplace(id, make_shared<WebRTCStreamer::Client>(id, ""));
        } else if (type == "answer") {
            // Push remote peer answer to our client class instance
            auto it = _clientsMap.get(id);
            if (it == nullptr)
                return;
            string sdp = json_msg["sdp"].get<string>();
            it->signalingSet(sdp);
        }
    };

    auto outputMessageHandler = [&](const string& id, const string& type,
            json json_msg)
    {
        string message = json_msg.dump();
        cout << "Signaling output message: '" << message << "'\n";

        if (type == "disconnect") {
            cout << "Deleting client peer " << id << "\n";
            _clientsMap.erase(id);
        } else {
            _signalingWS->send(message);
        }
    };

    // Signals processing loop
    while (!_do_term) {
        cout << "Signaling thread polling..." << "\n";
        optional<string> opt_message = _signalingQueue.pop();
        if (opt_message == nullopt)
            continue;

        optional<nlohmann::json> opt_json_msg = safeParse(opt_message.value());
        if (opt_json_msg == nullopt)
            continue;
        json json_msg = opt_json_msg.value();

        auto it = json_msg.find("io_mode");
        if (it == json_msg.end())
            continue;
        string ioMode = it->get<string>();

        it = json_msg.find("id");
        if (it == json_msg.end())
            continue;
        string id = it->get<string>();

        it = json_msg.find("type");
        if (it == json_msg.end())
            continue;
        string type = it->get<string>();

        if (ioMode == "input")
            inputMessageHandler(id, type, json_msg);
        else
            outputMessageHandler(id, type, json_msg);
    }

    signalingThreadOut.join();
}

void WebRTCStreamer::_pushData(void *const data, size_t size)
{
    if (data == nullptr || size == 0)
        return;

    struct timeval time;
    gettimeofday(&time, NULL);
    uint64_t sampleTime = uint64_t(time.tv_sec) * 1000 * 1000 + time.tv_usec;
    shared_ptr<Frame> frame = make_shared<Frame>(data, size, sampleTime);
    _framesQueueIn.push(frame);
}

void WebRTCStreamer::_multimediaThr()
{
    cout << "Launching multimedia thread..." << "\n";

    // Multimedia processing loop
    while (!_do_term) {
        optional<shared_ptr<Frame>> optFrame = _framesQueueIn.pop();
        if (optFrame == nullopt)
            continue;
        shared_ptr<Frame> frame = optFrame.value();

        // Itearte clients instances
        _clientsMap.traverse([&](string id, shared_ptr<Client> client){
            client->sendFrame(static_cast<const byte*>(frame->data),
                    frame->size, chrono::duration<double, micro>(
                    frame->sampleTime_usec));
        });
    }
}
