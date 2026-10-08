// Isochronous playback stream straight to the DAC, bypassing the Android
// audio stack (AudioFlinger mixer, resampler, volume curves, sound dose).
#pragma once

#include <libusb.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../dsp/ring_buffer.h"
#include "../uac/descriptors.h"
#include "packet_clock.h"

namespace engine {

struct StreamStats {
    uint64_t framesSent = 0;
    uint64_t underrunPackets = 0;
    uint64_t errors = 0;
    uint32_t feedbackQ16 = 0;
    uint32_t nominalQ16 = 0;
    uint32_t bufferedFrames = 0;
    uint32_t bufferCapacityFrames = 0;
    bool running = false;
};

class IsoStream {
public:
    IsoStream(libusb_context* ctx, libusb_device_handle* handle, const uac::StreamingAlt& alt, uint32_t rate,
              bool highSpeed);
    ~IsoStream();

    bool start(std::string& error);
    void stop();

    // Blocks until all bytes are queued, the stream stops or the timeout
    // expires. `bytes` must be a multiple of frameBytes(). Returns bytes queued.
    size_t write(const uint8_t* data, size_t bytes, int timeoutMs);
    void flush() { ring_.requestFlush(); }

    size_t frameBytes() const { return frameBytes_; }
    uint32_t rate() const { return rate_; }
    StreamStats stats() const;
    bool failed() const { return failed_.load(); }

private:
    static void LIBUSB_CALL onData(libusb_transfer* t);
    static void LIBUSB_CALL onFeedback(libusb_transfer* t);
    void fill(libusb_transfer* t);
    void finished();
    void eventLoop();

    libusb_context* ctx_;
    libusb_device_handle* handle_;
    uac::StreamingAlt alt_;
    uint32_t rate_;
    bool highSpeed_;
    size_t frameBytes_;
    uint32_t packetsPerSecond_ = 1000;
    uint32_t microframesPerPacket_ = 1;
    int packetsPerTransfer_ = 4;
    int maxPacketBytes_ = 0;

    PacketClock clock_;
    dsp::ByteRing ring_;

    std::vector<libusb_transfer*> transfers_;
    std::vector<std::unique_ptr<uint8_t[]>> buffers_;
    std::atomic<int> active_{0};
    std::atomic<bool> running_{false};
    std::atomic<bool> failed_{false};
    std::thread thread_;

    mutable std::mutex mutex_;
    std::condition_variable spaceCv_;

    std::atomic<uint64_t> framesSent_{0};
    std::atomic<uint64_t> underruns_{0};
    std::atomic<uint64_t> errors_{0};
    std::atomic<uint32_t> feedback_{0};
    bool lastHadData_ = false;  // event thread only
};

}  // namespace engine
