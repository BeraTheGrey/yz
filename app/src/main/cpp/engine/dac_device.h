// One opened USB audio device: descriptor model, hardware volume control and
// the optional exclusive (bit-perfect) playback stream.
#pragma once

#include <libusb.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../dsp/pcm.h"
#include "../dsp/resampler.h"
#include "../uac/control.h"
#include "../uac/descriptors.h"
#include "iso_stream.h"
#include "json.h"

namespace engine {

// How the Android kernel driver gets the device back after a one-shot volume change.
enum class ReattachMode : int {
    AttachDriver = 0,  // USBDEVFS_CONNECT on the control interface (default)
    UsbReset = 1,      // port reset, like usbDacVolumeAndroid does
};

class LibusbTransport : public uac::ControlTransport {
public:
    explicit LibusbTransport(libusb_device_handle* h) : h_(h) {}
    int control(uint8_t requestType, uint8_t request, uint16_t value, uint16_t index, uint8_t* data, uint16_t length,
                unsigned timeoutMs) override {
        return libusb_control_transfer(h_, requestType, request, value, index, data, length, timeoutMs);
    }

private:
    libusb_device_handle* h_;
};

class DacDevice {
public:
    static std::unique_ptr<DacDevice> open(int fd, const uint8_t* raw, size_t rawLength, std::string& error);
    ~DacDevice();

    std::string describeJson();
    std::string volumeJson(ReattachMode mode);
    std::string applyVolumeJson(bool toMax, int16_t target, bool allowBeyondMax, ReattachMode mode);
    std::string probeAboveMaxJson(int limitDb, bool keepHighest, ReattachMode mode);

    // Exclusive playback. startStream picks the best alt setting for the
    // source and returns a JSON description of the chosen path.
    std::string startStream(uint32_t sourceRate, int sourceChannels, int sourceBits, uint32_t forcedRate);
    // Returns frames consumed, or -1 when the stream is gone (unplugged / stopped).
    int write(const uint8_t* data, size_t bytes, dsp::InputEncoding enc, int channels, uint32_t rate);
    void flush();
    void stopStream();
    std::string statsJson();
    void setDigitalGain(float linear, bool softClip);
    bool streaming();

private:
    DacDevice() = default;

    // Detaches the kernel driver (snd-usb-audio) from every audio interface
    // and claims the control interface. No-op while streaming exclusively.
    bool claimControl(std::string& error);
    void releaseControl(ReattachMode mode);
    void detachAudioInterfaces();
    void reattach(ReattachMode mode);
    std::vector<int> audioInterfaces() const;

    void writeChannels(Json& j, const std::vector<uac::VolumeChannel>& chans);
    const uac::StreamingAlt* chooseAlt(uint32_t sourceRate, int sourceChannels, uint32_t forcedRate, uint32_t& rateOut);
    std::vector<uint32_t> ratesFor(const uac::StreamingAlt& alt);
    uint32_t packetsPerSecond(const uac::StreamingAlt& alt) const;
    bool highSpeed() const;
    void stopStreamLocked();

    libusb_context* ctx_ = nullptr;
    libusb_device_handle* handle_ = nullptr;
    uac::AudioFunction fn_;
    std::unique_ptr<LibusbTransport> transport_;
    std::unique_ptr<uac::AudioControl> control_;
    std::vector<std::vector<uint32_t>> uac2Rates_;  // per fn_.alts index, filled while claimed
    std::vector<uac::VolumeChannel> lastChannels_;   // last ranges read from the device

    std::mutex controlMutex_;  // serialises control requests / claiming
    bool exclusive_ = false;   // interfaces detached from the kernel for streaming
    std::vector<int> claimed_;

    std::mutex streamMutex_;
    std::shared_ptr<IsoStream> stream_;
    const uac::StreamingAlt* streamAlt_ = nullptr;

    // DSP state, only touched by the writer thread (and startStream).
    std::mutex dspMutex_;
    std::unique_ptr<dsp::Resampler> resampler_;
    dsp::Quantizer quantizer_;
    int sourceBits_ = 16;
    std::vector<float> floatBuf_;
    std::vector<float> resampled_;
    std::vector<uint8_t> outBuf_;
    std::atomic<float> gain_{1.0f};
    std::atomic<bool> softClip_{false};
};

}  // namespace engine
