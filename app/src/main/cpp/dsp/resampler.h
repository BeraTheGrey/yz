// Streaming polyphase windowed-sinc sample rate converter.
//
// Only used when the DAC cannot run at the source rate (e.g. a 96 kHz file on
// a dongle limited to 48 kHz). The step is tracked as an exact rational number,
// so there is no long-term drift.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dsp {

class Resampler {
public:
    Resampler(uint32_t inRate, uint32_t outRate, int channels, int halfTaps = 32, int phases = 256);

    // Consumes `frames` interleaved input frames and appends the produced
    // output frames to `out`.
    void process(const float* in, size_t frames, std::vector<float>& out);
    void reset();

    uint32_t inRate() const { return inRate_; }
    uint32_t outRate() const { return outRate_; }
    int channels() const { return channels_; }

private:
    uint32_t inRate_, outRate_;
    int channels_;
    int half_;
    int phases_;
    std::vector<float> table_;  // (phases_ + 1) rows of 2*half_ taps
    std::vector<float> buf_;    // interleaved history + pending input
    size_t pos_ = 0;            // integer input position (frames into buf_)
    uint64_t frac_ = 0;         // fractional position, numerator over outRate_
    uint64_t stepInt_ = 0;
    uint64_t stepFrac_ = 0;
};

}  // namespace dsp
