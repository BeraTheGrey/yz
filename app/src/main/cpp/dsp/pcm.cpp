#include "pcm.h"

#include <cmath>
#include <cstring>

namespace dsp {

int bytesPerSample(InputEncoding e) {
    switch (e) {
        case InputEncoding::Pcm8: return 1;
        case InputEncoding::Pcm16: return 2;
        case InputEncoding::Pcm24Packed: return 3;
        case InputEncoding::Pcm32:
        case InputEncoding::Float: return 4;
    }
    return 2;
}

void toFloat(const uint8_t* in, size_t samples, InputEncoding enc, float* out) {
    switch (enc) {
        case InputEncoding::Pcm8:
            for (size_t i = 0; i < samples; ++i) out[i] = (static_cast<int>(in[i]) - 128) * (1.0f / 128.0f);
            break;
        case InputEncoding::Pcm16:
            for (size_t i = 0; i < samples; ++i) {
                int16_t v = static_cast<int16_t>(in[2 * i] | (in[2 * i + 1] << 8));
                out[i] = v * (1.0f / 32768.0f);
            }
            break;
        case InputEncoding::Pcm24Packed:
            for (size_t i = 0; i < samples; ++i) {
                const uint8_t* p = in + 3 * i;
                int32_t v = (p[0] << 8) | (p[1] << 16) | (static_cast<int32_t>(p[2]) << 24);
                out[i] = static_cast<float>(v >> 8) * (1.0f / 8388608.0f);
            }
            break;
        case InputEncoding::Pcm32:
            for (size_t i = 0; i < samples; ++i) {
                const uint8_t* p = in + 4 * i;
                int32_t v = static_cast<int32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24));
                out[i] = static_cast<float>(static_cast<double>(v) * (1.0 / 2147483648.0));
            }
            break;
        case InputEncoding::Float:
            for (size_t i = 0; i < samples; ++i) {
                const uint8_t* p = in + 4 * i;
                uint32_t bits = p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
                float f;
                static_assert(sizeof(f) == sizeof(bits), "float must be 32-bit");
                std::memcpy(&f, &bits, sizeof(f));
                out[i] = std::isfinite(f) ? f : 0.0f;
            }
            break;
    }
}

Quantizer::Quantizer(OutputFormat fmt, int sourceBits) : fmt_(fmt) {
    if (fmt_.bitResolution <= 0 || fmt_.bitResolution > fmt_.containerBytes * 8) fmt_.bitResolution = fmt_.containerBytes * 8;
    dither_ = sourceBits > fmt_.bitResolution;
}

float Quantizer::tpdf() {
    // xorshift32; two uniform values give a triangular distribution in (-1, 1) LSB.
    auto next = [this]() {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return static_cast<float>(rng_) * (1.0f / 4294967296.0f);
    };
    return next() - next();
}

void Quantizer::process(const float* in, size_t frames, int inChannels, uint8_t* out) {
    const int bits = fmt_.bitResolution;
    const int shift = fmt_.containerBytes * 8 - bits;
    const double scale = std::ldexp(1.0, bits - 1);
    const int64_t maxV = static_cast<int64_t>(scale) - 1;
    const int64_t minV = -static_cast<int64_t>(scale);
    const int outCh = fmt_.channels;

    for (size_t f = 0; f < frames; ++f) {
        const float* src = in + f * inChannels;
        for (int c = 0; c < outCh; ++c) {
            float x;
            if (inChannels == 1) x = src[0];
            else if (outCh == 1) x = 0.5f * (src[0] + src[1]);
            else if (c < inChannels) x = src[c];
            else x = 0.0f;

            double v = static_cast<double>(x) * scale;
            if (dither_) v += tpdf();
            int64_t q = std::llrint(v);
            if (q > maxV) q = maxV;
            if (q < minV) q = minV;
            uint32_t word = static_cast<uint32_t>(static_cast<int64_t>(q) * (int64_t{1} << shift));
            for (int b = 0; b < fmt_.containerBytes; ++b) *out++ = static_cast<uint8_t>(word >> (8 * b));
        }
    }
}

float softClip(float x) {
    // Linear up to -1 dBFS, then a tanh knee (slope-continuous) that approaches
    // a -0.1 dBFS ceiling, leaving a little room for inter-sample peaks.
    constexpr float t = 0.891f;
    constexpr float ceiling = 0.989f;
    const float a = std::fabs(x);
    if (a <= t) return x;
    const float y = t + (ceiling - t) * std::tanh((a - t) / (ceiling - t));
    return x < 0 ? -y : y;
}

void applyGain(float* s, size_t n, float gain, bool clip) {
    if (gain == 1.0f && !clip) return;
    if (clip) {
        for (size_t i = 0; i < n; ++i) s[i] = softClip(s[i] * gain);
    } else {
        for (size_t i = 0; i < n; ++i) s[i] *= gain;
    }
}

}  // namespace dsp
