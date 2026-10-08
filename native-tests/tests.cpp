// Host unit tests for the parts of the native engine that do not need a real
// USB device: descriptor parsing, control request logic (against a simulated
// DAC), PCM conversion, resampling, ring buffer and packet timing.
//
//   cmake -S native-tests -B build/native-tests && cmake --build build/native-tests && build/native-tests/yzdac_tests
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include "dsp/pcm.h"
#include "dsp/resampler.h"
#include "dsp/ring_buffer.h"
#include "engine/json.h"
#include "engine/packet_clock.h"
#include "uac/control.h"
#include "uac/descriptors.h"

static int failures = 0;
static int checks = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        ++checks;                                                            \
        if (!(cond)) {                                                       \
            ++failures;                                                      \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

// ---------------------------------------------------------------------------
// Descriptor builders

using Bytes = std::vector<uint8_t>;

static void put(Bytes& b, std::initializer_list<int> v) {
    for (int x : v) b.push_back(static_cast<uint8_t>(x));
}

static void finishConfig(Bytes& cfg) {
    cfg[2] = static_cast<uint8_t>(cfg.size() & 0xFF);
    cfg[3] = static_cast<uint8_t>(cfg.size() >> 8);
}

// Topology modelled after the Apple USB-C to 3.5 mm adapter (05ac:110a):
// UAC1, playback IT(1, USB streaming) -> FU(2) -> OT(3, headphones) and a
// recording path IT(4, headset mic) -> FU(5) -> OT(6, USB streaming).
static Bytes appleLikeUac1() {
    Bytes b;
    put(b, {18, 0x01, 0x00, 0x02, 0, 0, 0, 64, 0xAC, 0x05, 0x0A, 0x11, 0x23, 0x01, 1, 2, 3, 1});  // device
    Bytes c;
    put(c, {9, 0x02, 0, 0, 3, 1, 0, 0x80, 50});  // config, 3 interfaces
    put(c, {9, 0x04, 0, 0, 0, 0x01, 0x01, 0x00, 0});  // AC interface
    put(c, {10, 0x24, 0x01, 0x00, 0x01, 0, 0, 2, 1, 2});  // header bcdADC 1.00 (length fixed below)
    put(c, {12, 0x24, 0x02, 1, 0x01, 0x01, 0, 2, 0x03, 0, 0, 0});  // IT 1 USB streaming
    put(c, {10, 0x24, 0x06, 2, 1, 1, 0x01, 0x02, 0x02, 0});  // FU 2: master mute, ch1 vol, ch2 vol
    put(c, {9, 0x24, 0x03, 3, 0x02, 0x03, 0, 2, 0});  // OT 3 headphones <- FU2
    put(c, {12, 0x24, 0x02, 4, 0x01, 0x02, 0, 1, 0, 0, 0, 0});  // IT 4 microphone
    put(c, {9, 0x24, 0x06, 5, 4, 1, 0x03, 0x00, 0});  // FU 5 (mic), master mute+vol
    put(c, {9, 0x24, 0x03, 6, 0x01, 0x01, 0, 5, 0});  // OT 6 USB streaming <- FU5
    // AS interface 1 (playback)
    put(c, {9, 0x04, 1, 0, 0, 0x01, 0x02, 0, 0});
    put(c, {9, 0x04, 1, 1, 1, 0x01, 0x02, 0, 0});
    put(c, {7, 0x24, 0x01, 1, 1, 0x01, 0x00});  // AS general, PCM
    put(c, {14, 0x24, 0x02, 0x01, 2, 3, 24, 2, 0x44, 0xAC, 0x00, 0x80, 0xBB, 0x00});  // 24-bit, 44.1/48k
    put(c, {9, 0x05, 0x01, 0x09, 0x28, 0x01, 1, 0, 0});  // EP1 OUT iso adaptive, 296 bytes
    put(c, {7, 0x25, 0x01, 0x01, 0, 0, 0});  // CS EP: sampling freq control
    put(c, {9, 0x04, 1, 2, 1, 0x01, 0x02, 0, 0});
    put(c, {7, 0x24, 0x01, 1, 1, 0x01, 0x00});
    put(c, {14, 0x24, 0x02, 0x01, 2, 2, 16, 2, 0x44, 0xAC, 0x00, 0x80, 0xBB, 0x00});  // 16-bit
    put(c, {9, 0x05, 0x01, 0x09, 0xC8, 0x00, 1, 0, 0});
    put(c, {7, 0x25, 0x01, 0x01, 0, 0, 0});
    // AS interface 2 (capture)
    put(c, {9, 0x04, 2, 0, 0, 0x01, 0x02, 0, 0});
    put(c, {9, 0x04, 2, 1, 1, 0x01, 0x02, 0, 0});
    put(c, {7, 0x24, 0x01, 6, 1, 0x01, 0x00});
    put(c, {11, 0x24, 0x02, 0x01, 1, 2, 16, 1, 0x80, 0xBB, 0x00});
    put(c, {9, 0x05, 0x82, 0x05, 0x64, 0x00, 1, 0, 0});
    put(c, {7, 0x25, 0x01, 0x01, 0, 0, 0});
    finishConfig(c);
    b.insert(b.end(), c.begin(), c.end());
    return b;
}

// Generic UAC2 high-speed DAC with explicit feedback and a programmable clock.
static Bytes genericUac2() {
    Bytes c;
    put(c, {9, 0x02, 0, 0, 2, 1, 0, 0x80, 50});
    put(c, {8, 0x0B, 0, 2, 0x01, 0x00, 0x20, 0});  // IAD
    put(c, {9, 0x04, 0, 0, 0, 0x01, 0x01, 0x20, 0});
    put(c, {9, 0x24, 0x01, 0x00, 0x02, 0x08, 0, 0, 0});  // header bcdADC 2.00
    put(c, {8, 0x24, 0x0A, 10, 0x03, 0x07, 0, 0});  // clock source 10, freq programmable
    put(c, {17, 0x24, 0x02, 1, 0x01, 0x01, 0, 10, 2, 3, 0, 0, 0, 0, 0, 0, 0});  // IT 1 USB streaming
    // FU 2: master vol (bits 2-3) + mute (bits 0-1), ch1/ch2 volume
    put(c, {18, 0x24, 0x06, 2, 1, 0x0F, 0, 0, 0, 0x0C, 0, 0, 0, 0x0C, 0, 0, 0, 0});
    put(c, {12, 0x24, 0x03, 3, 0x01, 0x03, 0, 2, 10, 0, 0, 0});  // OT 3 speaker
    put(c, {9, 0x04, 1, 0, 0, 0x01, 0x02, 0x20, 0});
    put(c, {9, 0x04, 1, 1, 2, 0x01, 0x02, 0x20, 0});
    put(c, {16, 0x24, 0x01, 1, 0, 0x01, 0x01, 0, 0, 0, 2, 3, 0, 0, 0, 0});
    put(c, {6, 0x24, 0x02, 0x01, 4, 24});  // 24 bits in 4 bytes
    put(c, {7, 0x05, 0x01, 0x05, 0x00, 0x02, 1});  // EP1 OUT async 512
    put(c, {8, 0x25, 0x01, 0, 0, 0, 0, 0});
    put(c, {7, 0x05, 0x81, 0x11, 0x04, 0x00, 4});  // EP1 IN feedback
    finishConfig(c);
    return c;
}

// ---------------------------------------------------------------------------
// Simulated DAC

struct FakeDac : uac::ControlTransport {
    bool uac2 = false;
    int16_t min = -0x3C00, max = 0, res = 0x0100;
    int16_t clampMax = 0;  // device latches up to this value
    std::map<uint32_t, int16_t> volume;
    std::map<uint32_t, uint8_t> mute;
    std::vector<uint32_t> rates = {44100, 48000, 96000};
    uint32_t currentRate = 0;
    int calls = 0;

    int control(uint8_t rt, uint8_t rq, uint16_t value, uint16_t index, uint8_t* d, uint16_t len, unsigned) override {
        ++calls;
        const uint8_t cs = value >> 8, ch = value & 0xFF, unit = index >> 8;
        const uint32_t key = (unit << 8) | ch;
        const bool in = rt & 0x80;
        if ((rt & 0x1F) == 0x02) {  // endpoint: UAC1 sampling frequency
            if (in) { d[0] = currentRate; d[1] = currentRate >> 8; d[2] = currentRate >> 16; return 3; }
            currentRate = d[0] | (d[1] << 8) | (d[2] << 16);
            return len;
        }
        if (uac2 && unit == 10 && cs == 0x01) {  // clock
            if (rq == 0x02) {
                const uint16_t n = rates.size();
                std::vector<uint8_t> r = {static_cast<uint8_t>(n), static_cast<uint8_t>(n >> 8)};
                for (uint32_t x : rates)
                    for (int k = 0; k < 3; ++k) {
                        uint32_t v = k == 2 ? 0 : x;
                        for (int i = 0; i < 4; ++i) r.push_back(v >> (8 * i));
                    }
                const int c = std::min<int>(len, r.size());
                std::memcpy(d, r.data(), c);
                return c;
            }
            if (in) { for (int i = 0; i < 4; ++i) d[i] = currentRate >> (8 * i); return 4; }
            currentRate = d[0] | (d[1] << 8) | (d[2] << 16) | (d[3] << 24);
            return len;
        }
        if (cs == 0x01) {  // mute
            if (in) { d[0] = mute[key]; return 1; }
            mute[key] = d[0];
            return 1;
        }
        if (cs != 0x02) return -9;  // stall
        auto w16 = [&](int16_t v) { d[0] = v & 0xFF; d[1] = (v >> 8) & 0xFF; return 2; };
        if (!in) {
            int16_t v = static_cast<int16_t>(d[0] | (d[1] << 8));
            volume[key] = std::max<int16_t>(min, std::min<int16_t>(clampMax, v));
            return 2;
        }
        if (uac2) {
            if (rq == 0x01) return w16(volume.count(key) ? volume[key] : min);
            if (rq == 0x02 && len >= 8) {
                d[0] = 1; d[1] = 0;
                d[2] = min & 0xFF; d[3] = (min >> 8) & 0xFF;
                d[4] = max & 0xFF; d[5] = (max >> 8) & 0xFF;
                d[6] = res & 0xFF; d[7] = (res >> 8) & 0xFF;
                return 8;
            }
            return -9;
        }
        switch (rq) {
            case 0x81: return w16(volume.count(key) ? volume[key] : min);
            case 0x82: return w16(min);
            case 0x83: return w16(max);
            case 0x84: return w16(res);
        }
        return -9;
    }
};

// ---------------------------------------------------------------------------

static void testAppleDescriptor() {
    Bytes raw = appleLikeUac1();
    uac::AudioFunction f;
    std::string err;
    CHECK(uac::parse(raw.data(), raw.size(), f, &err));
    CHECK(f.version == uac::Version::UAC1);
    CHECK(f.vendorId == 0x05AC && f.productId == 0x110A);
    CHECK(f.controlInterface == 0);
    auto fus = f.playbackFeatureUnits();
    CHECK(fus.size() == 1 && fus[0] == 2);  // the mic FU 5 must not be touched
    const uac::FeatureUnit* fu = f.featureUnit(2);
    CHECK(fu && fu->channelCount() == 2);
    CHECK(fu && !fu->volume[0] && fu->volume[1] && fu->volume[2] && fu->mute[0]);
    auto alts = f.playbackAlts();
    CHECK(alts.size() == 2);
    CHECK(alts[0]->bitResolution == 24 && alts[0]->subslotBytes == 3 && alts[0]->channels == 2);
    CHECK(alts[0]->rates.size() == 2 && alts[0]->supportsRate(48000) && alts[0]->supportsRate(44100));
    CHECK(alts[0]->data.address == 0x01 && alts[0]->data.syncType() == 2);
    CHECK(alts[0]->sampleRateControl);
    CHECK(f.alts.size() == 3 && !f.alts[2].isPlayback());
}

static void testUac2Descriptor() {
    Bytes raw = genericUac2();
    uac::AudioFunction f;
    std::string err;
    CHECK(uac::parse(raw.data(), raw.size(), f, &err));
    CHECK(f.version == uac::Version::UAC2);
    auto fus = f.playbackFeatureUnits();
    CHECK(fus.size() == 1 && fus[0] == 2);
    const uac::FeatureUnit* fu = f.featureUnit(2);
    CHECK(fu && fu->volume[0] && fu->mute[0] && fu->volume[1] && fu->volume[2] && !fu->mute[1]);
    auto alts = f.playbackAlts();
    CHECK(alts.size() == 1);
    CHECK(alts[0]->subslotBytes == 4 && alts[0]->bitResolution == 24 && alts[0]->channels == 2);
    CHECK(alts[0]->feedback.valid() && alts[0]->feedback.address == 0x81);
    CHECK(alts[0]->data.syncType() == 1);
    CHECK(f.clockSourceFor(*alts[0]) == 10);
    CHECK(f.clock(10) && f.clock(10)->frequencyProgrammable());
}

static void testTruncatedDescriptor() {
    Bytes raw = appleLikeUac1();
    for (size_t cut = 0; cut < raw.size(); cut += 7) {
        uac::AudioFunction f;
        uac::parse(raw.data(), cut, f, nullptr);  // must not crash
    }
    uac::AudioFunction f;
    std::string err;
    const uint8_t junk[] = {0, 0, 0};
    CHECK(!uac::parse(junk, sizeof(junk), f, &err));
    CHECK(!err.empty());
}

static void testUac1VolumeMax() {
    Bytes raw = appleLikeUac1();
    uac::AudioFunction f;
    uac::parse(raw.data(), raw.size(), f, nullptr);
    FakeDac dac;
    dac.min = -0x4000;
    dac.max = 0;
    dac.clampMax = 0;
    dac.volume[(2 << 8) | 1] = -0x2000;  // dongle boots at -32 dB
    dac.volume[(2 << 8) | 2] = -0x2000;
    dac.mute[(2 << 8) | 0] = 1;
    uac::AudioControl ctl(dac, f);

    auto chans = ctl.volumeChannels();
    // ch1, ch2 volume + master mute-only entry
    CHECK(chans.size() == 3);
    CHECK(chans[0].rangeValid && chans[0].max == 0 && chans[0].min == -0x4000 && chans[0].cur == -0x2000);

    auto res = ctl.applyVolume(true, 0, false);
    CHECK(res.size() == 2);
    for (auto& r : res) CHECK(r.ok && r.readBackValid && r.readBack == 0 && r.requested == 0);
    CHECK(dac.mute[(2 << 8) | 0] == 0);
    CHECK(dac.volume.count((5 << 8) | 0) == 0);  // microphone FU untouched

    // Clamped explicit value
    res = ctl.applyVolume(false, -0x7000, false);
    for (auto& r : res) CHECK(r.requested == -0x4000);
}

static void testUac2VolumeAndProbe() {
    Bytes raw = genericUac2();
    uac::AudioFunction f;
    uac::parse(raw.data(), raw.size(), f, nullptr);
    FakeDac dac;
    dac.uac2 = true;
    dac.min = -0x6000;
    dac.max = -0x0600;          // reports -6 dB as maximum ...
    dac.clampMax = 0x0000;      // ... but actually latches up to 0 dB
    uac::AudioControl ctl(dac, f);

    auto chans = ctl.volumeChannels();
    CHECK(chans.size() == 3);  // master + 2 channels
    CHECK(chans[0].channel == 0 && chans[0].rangeValid && chans[0].max == -0x0600);

    auto res = ctl.applyVolume(true, 0, false);
    CHECK(res.size() == 3);
    for (auto& r : res) CHECK(r.ok && r.readBack == -0x0600);

    auto probe = ctl.probeAboveMax(12, false);
    CHECK(probe.size() == 3);
    for (auto& p : probe) CHECK(p.aboveMax && p.highestAccepted == 0);
    // restored to the reported maximum
    CHECK(dac.volume[(2 << 8) | 1] == -0x0600);

    auto keep = ctl.probeAboveMax(12, true);
    CHECK(dac.volume[(2 << 8) | 1] == 0);

    auto rates = ctl.querySampleRates(*f.playbackAlts()[0]);
    CHECK(rates.size() == 3 && rates[0] == 44100 && rates[2] == 96000);
    CHECK(ctl.setSampleRate(*f.playbackAlts()[0], 96000) && dac.currentRate == 96000);
    uint32_t rate = 0;
    CHECK(ctl.getSampleRate(*f.playbackAlts()[0], rate) && rate == 96000);
}

static void testProbeOnHonestDevice() {
    Bytes raw = appleLikeUac1();
    uac::AudioFunction f;
    uac::parse(raw.data(), raw.size(), f, nullptr);
    FakeDac dac;
    dac.max = 0;
    dac.clampMax = 0;
    uac::AudioControl ctl(dac, f);
    auto probe = ctl.probeAboveMax(12, true);
    CHECK(probe.size() == 2);
    for (auto& p : probe) CHECK(!p.aboveMax && p.highestAccepted == 0);
    CHECK(dac.calls < 80);  // stops at the first clamped step
}

static void testUac1SampleRate() {
    Bytes raw = appleLikeUac1();
    uac::AudioFunction f;
    uac::parse(raw.data(), raw.size(), f, nullptr);
    FakeDac dac;
    uac::AudioControl ctl(dac, f);
    CHECK(ctl.setSampleRate(*f.playbackAlts()[0], 44100));
    CHECK(dac.currentRate == 44100);
}

static void testBitPerfectConversion() {
    // 16-bit source into a 24-bit/3-byte DAC slot: exact zero padding.
    const int16_t src[] = {0, 1, -1, 32767, -32768, 12345};
    float f[6];
    dsp::toFloat(reinterpret_cast<const uint8_t*>(src), 6, dsp::InputEncoding::Pcm16, f);
    dsp::Quantizer q({2, 3, 24}, 16);
    CHECK(!q.dithering());
    uint8_t out[18];
    q.process(f, 3, 2, out);
    for (int i = 0; i < 6; ++i) {
        int32_t v = (out[3 * i] << 8) | (out[3 * i + 1] << 16) | (out[3 * i + 2] << 24);
        CHECK((v >> 8) == src[i] * 256);
    }

    // 24-bit packed source through float into a 24-in-32 slot (MSB aligned).
    const int32_t vals[] = {0, 1, -1, 8388607, -8388608, 0x123456};
    uint8_t packed[18];
    for (int i = 0; i < 6; ++i) {
        packed[3 * i] = vals[i] & 0xFF;
        packed[3 * i + 1] = (vals[i] >> 8) & 0xFF;
        packed[3 * i + 2] = (vals[i] >> 16) & 0xFF;
    }
    dsp::toFloat(packed, 6, dsp::InputEncoding::Pcm24Packed, f);
    dsp::Quantizer q32({2, 4, 24}, 24);
    uint8_t out32[24];
    q32.process(f, 3, 2, out32);
    for (int i = 0; i < 6; ++i) {
        int32_t v;
        std::memcpy(&v, out32 + 4 * i, 4);
        CHECK((v & 0xFF) == 0);
        CHECK((v >> 8) == vals[i]);
    }

    // 16-bit into a 16-bit DAC: identity.
    dsp::toFloat(reinterpret_cast<const uint8_t*>(src), 6, dsp::InputEncoding::Pcm16, f);
    dsp::Quantizer q16({2, 2, 16}, 16);
    int16_t o16[6];
    q16.process(f, 3, 2, reinterpret_cast<uint8_t*>(o16));
    CHECK(std::memcmp(o16, src, sizeof(src)) == 0);

    // Clipping of out-of-range float input.
    float hot[2] = {1.5f, -1.5f};
    q16.process(hot, 1, 2, reinterpret_cast<uint8_t*>(o16));
    CHECK(o16[0] == 32767 && o16[1] == -32768);

    // Mono source is duplicated to both channels.
    float mono[2] = {0.5f, -0.25f};
    int16_t st[4];
    q16.process(mono, 2, 1, reinterpret_cast<uint8_t*>(st));
    CHECK(st[0] == 16384 && st[1] == 16384 && st[2] == -8192 && st[3] == -8192);

    // 24-bit source into 16-bit DAC uses dither.
    dsp::Quantizer qd({2, 2, 16}, 24);
    CHECK(qd.dithering());
}

static void testSoftClip() {
    CHECK(dsp::softClip(0.5f) == 0.5f);
    CHECK(dsp::softClip(4.0f) < 1.0f && dsp::softClip(4.0f) > 0.95f);
    CHECK(dsp::softClip(-4.0f) > -1.0f);
    float s[3] = {0.1f, 0.2f, 0.3f};
    dsp::applyGain(s, 3, 1.0f, false);
    CHECK(s[0] == 0.1f && s[2] == 0.3f);
}

static double toneError(uint32_t inRate, uint32_t outRate, double freq) {
    dsp::Resampler rs(inRate, outRate, 2);
    const size_t n = inRate;  // one second
    std::vector<float> in(n * 2);
    for (size_t i = 0; i < n; ++i) {
        float v = static_cast<float>(0.5 * std::sin(2 * M_PI * freq * i / inRate));
        in[2 * i] = v;
        in[2 * i + 1] = -v;
    }
    std::vector<float> out;
    for (size_t off = 0; off < n; off += 997) {  // odd chunk sizes
        const size_t c = std::min<size_t>(997, n - off);
        rs.process(in.data() + off * 2, c, out);
    }
    const size_t frames = out.size() / 2;
    // Output length close to the exact ratio (minus the filter tail still buffered).
    const double expected = static_cast<double>(n) * outRate / inRate;
    if (std::fabs(frames - expected) > 40) return 1.0;
    // Compare against the ideal signal (no group delay because of the centred pre-roll).
    double err = 0;
    size_t cnt = 0;
    for (size_t i = 200; i + 200 < frames; ++i) {
        double ideal = 0.5 * std::sin(2 * M_PI * freq * static_cast<double>(i) / outRate);
        err = std::max(err, std::fabs(out[2 * i] - ideal));
        err = std::max(err, std::fabs(out[2 * i + 1] + ideal));
        ++cnt;
    }
    return cnt ? err : 1.0;
}

static void testResampler() {
    CHECK(toneError(96000, 48000, 1000) < 2e-3);
    CHECK(toneError(44100, 48000, 1000) < 2e-3);
    CHECK(toneError(48000, 44100, 3000) < 2e-3);
    CHECK(toneError(192000, 44100, 1000) < 2e-3);
    CHECK(toneError(88200, 44100, 10000) < 5e-3);
}

static void testResamplerAntiAlias() {
    // A 30 kHz tone in a 96 kHz stream must be removed when converting to 48 kHz.
    dsp::Resampler rs(96000, 48000, 1);
    std::vector<float> in(96000);
    for (size_t i = 0; i < in.size(); ++i) in[i] = static_cast<float>(0.5 * std::sin(2 * M_PI * 30000.0 * i / 96000));
    std::vector<float> out;
    rs.process(in.data(), in.size(), out);
    double peak = 0;
    for (size_t i = 500; i + 500 < out.size(); ++i) peak = std::max(peak, static_cast<double>(std::fabs(out[i])));
    CHECK(peak < 0.5 * 0.01);  // > 40 dB rejection
}

static void testRing() {
    dsp::ByteRing r(10);
    uint8_t a[8] = {1, 2, 3, 4, 5, 6, 7, 8}, b[16] = {};
    CHECK(r.write(a, 8) == 8);
    CHECK(r.space() == 2);
    CHECK(r.write(a, 8) == 2);
    CHECK(r.read(b, 6) == 6 && b[0] == 1 && b[5] == 6);
    CHECK(r.write(a, 4) == 4);  // wraps
    CHECK(r.read(b, 16) == 8);
    CHECK(b[0] == 7 && b[1] == 8 && b[2] == 1 && b[3] == 2 && b[4] == 1 && b[7] == 4);
    r.write(a, 5);
    r.requestFlush();
    CHECK(r.read(b, 16) == 0);
    CHECK(r.available() == 0);
}

static void testPacketClock() {
    // 44.1 kHz at full speed: 44.1 frames per 1 ms packet.
    engine::PacketClock pc(engine::nominalFramesQ16(44100, 1000));
    uint64_t total = 0;
    for (int i = 0; i < 1000; ++i) {
        uint32_t f = pc.next(100);
        CHECK(f == 44 || f == 45);
        total += f;
    }
    CHECK(total >= 44099 && total <= 44100);

    // High speed 48 kHz, bInterval 1: 6 frames per microframe.
    engine::PacketClock hs(engine::nominalFramesQ16(48000, 8000));
    for (int i = 0; i < 16; ++i) CHECK(hs.next(100) == 6);

    // Feedback decoding.
    const uint32_t nomFs = engine::nominalFramesQ16(48000, 1000);  // 48.0
    uint32_t v1014 = 48u << 14;
    uint8_t fs[3] = {static_cast<uint8_t>(v1014), static_cast<uint8_t>(v1014 >> 8), static_cast<uint8_t>(v1014 >> 16)};
    CHECK(engine::decodeFeedback(fs, 3, nomFs, 1) == (48u << 16));
    uint32_t v1616 = (48u << 16) + 0x1000;  // FS device wrongly sending 16.16
    uint8_t fs4[4] = {static_cast<uint8_t>(v1616), static_cast<uint8_t>(v1616 >> 8), static_cast<uint8_t>(v1616 >> 16),
                      static_cast<uint8_t>(v1616 >> 24)};
    CHECK(engine::decodeFeedback(fs4, 4, nomFs, 1) == v1616);
    const uint32_t nomHs = engine::nominalFramesQ16(44100, 8000);  // 5.5125
    uint32_t hsv = static_cast<uint32_t>(5.5125 * 65536);
    uint8_t h4[4] = {static_cast<uint8_t>(hsv), static_cast<uint8_t>(hsv >> 8), static_cast<uint8_t>(hsv >> 16), 0};
    CHECK(engine::decodeFeedback(h4, 4, nomHs, 1) == hsv);
    uint8_t zero[4] = {0, 0, 0, 0};
    CHECK(engine::decodeFeedback(zero, 4, nomHs, 1) == 0);
    uint8_t crazy[4] = {0, 0, 0, 0x40};
    CHECK(engine::decodeFeedback(crazy, 4, nomHs, 1) == 0);
}

static void testJson() {
    engine::Json j;
    j.beginObject().field("a", 1).field("s", "x\"y").key("arr").beginArray();
    j.value(1).value(2);
    j.beginObject().field("k", true).endObject();
    j.endArray().field("d", 0.5).endObject();
    CHECK(j.str() == "{\"a\":1,\"s\":\"x\\\"y\",\"arr\":[1,2,{\"k\":true}],\"d\":0.5000}");
}

int main() {
    testAppleDescriptor();
    testUac2Descriptor();
    testTruncatedDescriptor();
    testUac1VolumeMax();
    testUac2VolumeAndProbe();
    testProbeOnHonestDevice();
    testUac1SampleRate();
    testBitPerfectConversion();
    testSoftClip();
    testResampler();
    testResamplerAntiAlias();
    testRing();
    testPacketClock();
    testJson();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
