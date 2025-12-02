/**
 */

#include <thread>
#include <atomic>
#include <rtc/rtc.hpp>

using namespace std;

class VideoProducer {
public:
    VideoProducer(function<void(const uint8_t *data, size_t size)> onSample,
            string inputUrl = "");
    ~VideoProducer();

private:
    void dummyStreamThr();
    function<void (const uint8_t *data, size_t size)> sampleHandler;
    string inputUrl;
    thread _videoThread;
    atomic_bool _do_term;
};
