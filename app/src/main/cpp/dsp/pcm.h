// PCM conversion helpers.
//
// The pipeline is float32 internally. That is lossless for 16- and 24-bit
// sources, so with unity gain and no resampling the samples written to the DAC
// are bit-identical to the source (16-bit is zero padded into wider slots).
#pragma once

#include <cstddef>
#include <cstdint>

namespace dsp {

// Values match PcmEncoding in NativeBridge.kt.
enum class InputEncoding : int { Pcm16 = 0, Float = 1, Pcm24Packed = 2, Pcm32 = 3, Pcm8 = 4 };

int bytesPerSample(InputEncoding e);

// Interleaved input -> interleaved float in [-1, 1).
void toFloat(const uint8_t* in, size_t samples, InputEncoding enc, float* out);

struct OutputFormat {
    int channels = 2;
    int containerBytes = 2;  // UAC subslot size
    int bitResolution = 16;  // valid bits, MSB aligned inside the container
};

class Quantizer {
public:
    Quantizer() = default;
    Quantizer(OutputFormat fmt, int sourceBits);

    // Converts `frames` interleaved float frames with `inChannels` channels to
    // the device format, mapping channels (mono is duplicated, extra channels
    // are dropped, missing ones are silent). TPDF dither is applied only when
    // the device has fewer bits than the source.
    void process(const float* in, size_t frames, int inChannels, uint8_t* out);

    size_t frameBytes() const { return static_cast<size_t>(fmt_.channels) * fmt_.containerBytes; }
    bool dithering() const { return dither_; }

private:
    float tpdf();

    OutputFormat fmt_;
    bool dither_ = false;
    uint32_t rng_ = 0x12345678u;
};

// Applies linear gain and, optionally, a soft clipper that keeps peaks above
// -1 dBFS from hard clipping when a pre-gain boost is used.
void applyGain(float* samples, size_t count, float gain, bool softClip);

float softClip(float x);

}  // namespace dsp
