/**
 */

#include <atomic>
#include <signal.h>
#include <rtc/rtc.hpp>
#include "VideoProducer.h"
#include "WebRTCStreamer.h"

using namespace std;
using namespace rtc;

static volatile sig_atomic_t do_term = 0;
static void sig_handler_term(int signum)
{
    do_term = 1;
}

int main(int argc, char **argv) try
{
    sigset_t set;
    sigfillset(&set);
    sigdelset(&set, SIGTERM);
    sigdelset(&set, SIGINT);
    pthread_sigmask(SIG_SETMASK, &set, NULL);
    signal(SIGTERM, sig_handler_term);
    signal(SIGINT, sig_handler_term);

    InitLogger(LogLevel::Debug);
    string localId = "server";
    cout << "The local ID is: " << localId << endl;

    std::shared_ptr<WebRTCStreamer> streamer = make_shared<WebRTCStreamer>(
            "ws://127.0.0.1:8000/" + localId);
    VideoProducer videoSource([&](const uint8_t *data, size_t size) {
        streamer->pushData((void*const)data, size);
    });

    while (!do_term)
        this_thread::sleep_for(chrono::seconds(1));

    cout << "Cleaning up..." << endl;
    return 0;
} catch (const std::exception &e) {
    std::cout << "Error: " << e.what() << std::endl;
    return -1;
}
