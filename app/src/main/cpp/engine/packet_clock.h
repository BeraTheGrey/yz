// Isochronous packet sizing and feedback decoding (no libusb dependency).
#pragma once

#include <cstdint>
#include <cstdlib>

namespace engine {

// Frames per packet in Q16.16, derived from the sample rate and how many
// packets the endpoint is serviced per second.
inline uint32_t nominalFramesQ16(uint32_t rate, uint32_t packetsPerSecond) {
    return static_cast<uint32_t>((static_cast<uint64_t>(rate) << 16) / packetsPerSecond);
}

// Decodes an explicit feedback value into Q16.16 frames per packet.
//
// Full speed devices are supposed to send 10.14 in 3 bytes and high speed
// devices 16.16 per microframe in 4 bytes, but many get this wrong. As the
// Linux driver does, the plausible interpretation closest to the nominal value
// (within +-25 %) is taken; 0 means "ignore this sample".
inline uint32_t decodeFeedback(const uint8_t* p, int len, uint32_t nominalQ16, uint32_t microframesPerPacket) {
    if (len < 3) return 0;
    uint64_t raw = p[0] | (p[1] << 8) | (static_cast<uint32_t>(p[2]) << 16);
    if (len >= 4) raw |= static_cast<uint64_t>(p[3]) << 24;
    if (raw == 0) return 0;
    const uint64_t mpp = microframesPerPacket ? microframesPerPacket : 1;
    const uint64_t candidates[] = {raw, raw << 2, raw >> 2, raw * mpp, (raw << 2) * mpp};
    uint64_t best = 0;
    uint64_t bestDiff = UINT64_MAX;
    for (uint64_t c : candidates) {
        const uint64_t diff = c > nominalQ16 ? c - nominalQ16 : nominalQ16 - c;
        if (diff < bestDiff) {
            bestDiff = diff;
            best = c;
        }
    }
    if (bestDiff * 4 > nominalQ16) return 0;
    return static_cast<uint32_t>(best);
}

// Turns a fractional frames-per-packet rate into an integer sequence
// (e.g. 44.1 frames/ms -> nine packets of 44 and one of 45).
class PacketClock {
public:
    explicit PacketClock(uint32_t nominalQ16 = 0) : nominal_(nominalQ16), rate_(nominalQ16) {}

    void setFeedback(uint32_t q16) {
        if (q16) rate_ = q16;
    }
    uint32_t rateQ16() const { return rate_; }
    uint32_t nominalQ16() const { return nominal_; }

    uint32_t next(uint32_t maxFrames) {
        acc_ += rate_;
        uint32_t frames = acc_ >> 16;
        acc_ &= 0xFFFF;
        if (frames > maxFrames) frames = maxFrames;
        return frames;
    }

private:
    uint32_t nominal_;
    uint32_t rate_;
    uint32_t acc_ = 0;
};

}  // namespace engine
