// UAC class-specific control requests: hardware volume, mute and sample rate.
//
// The transport is abstract so the logic can be exercised against a simulated
// device in host unit tests; on Android it is backed by libusb.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "descriptors.h"

namespace uac {

class ControlTransport {
public:
    virtual ~ControlTransport() = default;
    // Same contract as libusb_control_transfer: bytes transferred or < 0 on error.
    virtual int control(uint8_t requestType, uint8_t request, uint16_t value, uint16_t index, uint8_t* data,
                        uint16_t length, unsigned timeoutMs) = 0;
};

// Volume values are in UAC units: signed 1/256 dB.
constexpr int16_t kDb = 256;

struct VolumeChannel {
    uint8_t unit = 0;
    uint8_t channel = 0;  // 0 = master
    bool rangeValid = false;
    int16_t min = 0;
    int16_t max = 0;
    int16_t res = 0;
    int16_t cur = 0;
    bool hasMute = false;
};

struct VolumeResult {
    VolumeChannel channel;
    int16_t requested = 0;
    bool ok = false;
    int16_t readBack = 0;
    bool readBackValid = false;
};

struct ProbeResult {
    VolumeChannel channel;
    int16_t highestAccepted = 0;  // highest value the device reported back
    bool aboveMax = false;        // device accepted something above GET_MAX
};

class AudioControl {
public:
    AudioControl(ControlTransport& transport, const AudioFunction& function);

    // Playback volume channels (master + logical) with their current ranges.
    std::vector<VolumeChannel> volumeChannels();

    bool getVolume(uint8_t unit, uint8_t channel, int16_t& value);
    bool setVolume(uint8_t unit, uint8_t channel, int16_t value);
    bool queryRange(VolumeChannel& ch);
    bool setMute(uint8_t unit, uint8_t channel, bool mute);

    // Sets every playback volume channel. With toMax the device's own GET_MAX
    // (or RANGE max) is used, otherwise `target` clamped to the range unless
    // allowBeyondMax is set. Channels are also unmuted.
    std::vector<VolumeResult> applyVolume(bool toMax, int16_t target, bool allowBeyondMax);

    // Experimental: tries values above the reported maximum in 1 dB steps (up
    // to `limitDb`) and records what the device actually latches. If
    // keepHighest is false every channel is restored to its reported max.
    std::vector<ProbeResult> probeAboveMax(int limitDb, bool keepHighest);

    // UAC2: discrete rates supported by the clock feeding `alt`.
    std::vector<uint32_t> querySampleRates(const StreamingAlt& alt);
    bool setSampleRate(const StreamingAlt& alt, uint32_t rate);
    bool getSampleRate(const StreamingAlt& alt, uint32_t& rate);

    const AudioFunction& function() const { return fn_; }

private:
    int in(uint8_t request, uint16_t value, uint16_t index, uint8_t* data, uint16_t len, bool endpoint = false);
    int out(uint8_t request, uint16_t value, uint16_t index, uint8_t* data, uint16_t len, bool endpoint = false);
    uint16_t unitIndex(uint8_t unit) const { return static_cast<uint16_t>((unit << 8) | fn_.controlInterface); }

    ControlTransport& t_;
    const AudioFunction& fn_;
};

}  // namespace uac
