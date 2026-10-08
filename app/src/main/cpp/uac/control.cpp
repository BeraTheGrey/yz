#include "control.h"

#include <algorithm>
#include <cstdint>

namespace uac {
namespace {

constexpr uint8_t kReqTypeIfOut = 0x21;  // class | interface | host-to-device
constexpr uint8_t kReqTypeIfIn = 0xA1;
constexpr uint8_t kReqTypeEpOut = 0x22;  // class | endpoint
constexpr uint8_t kReqTypeEpIn = 0xA2;

// UAC1 request codes
constexpr uint8_t kSetCur = 0x01;
constexpr uint8_t kGetCur = 0x81;
constexpr uint8_t kGetMin = 0x82;
constexpr uint8_t kGetMax = 0x83;
constexpr uint8_t kGetRes = 0x84;
// UAC2 request codes
constexpr uint8_t kCur = 0x01;
constexpr uint8_t kRange = 0x02;

constexpr uint8_t kCsMute = 0x01;
constexpr uint8_t kCsVolume = 0x02;
constexpr uint8_t kCsSamplingFreq = 0x01;  // UAC1 endpoint control / UAC2 clock control

constexpr unsigned kTimeoutMs = 1000;

int16_t s16(const uint8_t* p) { return static_cast<int16_t>(p[0] | (p[1] << 8)); }
uint32_t u32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

AudioControl::AudioControl(ControlTransport& transport, const AudioFunction& function)
    : t_(transport), fn_(function) {}

int AudioControl::in(uint8_t request, uint16_t value, uint16_t index, uint8_t* data, uint16_t len, bool endpoint) {
    return t_.control(endpoint ? kReqTypeEpIn : kReqTypeIfIn, request, value, index, data, len, kTimeoutMs);
}

int AudioControl::out(uint8_t request, uint16_t value, uint16_t index, uint8_t* data, uint16_t len, bool endpoint) {
    return t_.control(endpoint ? kReqTypeEpOut : kReqTypeIfOut, request, value, index, data, len, kTimeoutMs);
}

bool AudioControl::getVolume(uint8_t unit, uint8_t channel, int16_t& value) {
    uint8_t buf[2] = {0, 0};
    // UAC2 uses the same bRequest (CUR) for get and set; direction comes from bmRequestType.
    const uint8_t req = fn_.version == Version::UAC2 ? kCur : kGetCur;
    int r = in(req, (kCsVolume << 8) | channel, unitIndex(unit), buf, 2);
    if (r < 2) return false;
    value = s16(buf);
    return true;
}

bool AudioControl::setVolume(uint8_t unit, uint8_t channel, int16_t value) {
    uint8_t buf[2] = {static_cast<uint8_t>(value & 0xFF), static_cast<uint8_t>((value >> 8) & 0xFF)};
    const uint8_t req = fn_.version == Version::UAC2 ? kCur : kSetCur;
    return out(req, (kCsVolume << 8) | channel, unitIndex(unit), buf, 2) >= 0;
}

bool AudioControl::setMute(uint8_t unit, uint8_t channel, bool mute) {
    uint8_t buf[1] = {static_cast<uint8_t>(mute ? 1 : 0)};
    const uint8_t req = fn_.version == Version::UAC2 ? kCur : kSetCur;
    return out(req, (kCsMute << 8) | channel, unitIndex(unit), buf, 1) >= 0;
}

bool AudioControl::queryRange(VolumeChannel& ch) {
    const uint16_t value = (kCsVolume << 8) | ch.channel;
    const uint16_t index = unitIndex(ch.unit);
    ch.rangeValid = false;
    if (fn_.version == Version::UAC2) {
        uint8_t buf[8] = {};
        int r = in(kRange, value, index, buf, sizeof(buf));
        if (r < 8 || (buf[0] | (buf[1] << 8)) < 1) return false;
        ch.min = s16(buf + 2);
        ch.max = s16(buf + 4);
        ch.res = s16(buf + 6);
    } else {
        uint8_t buf[2];
        if (in(kGetMin, value, index, buf, 2) < 2) return false;
        ch.min = s16(buf);
        if (in(kGetMax, value, index, buf, 2) < 2) return false;
        ch.max = s16(buf);
        ch.res = in(kGetRes, value, index, buf, 2) >= 2 ? s16(buf) : 1;
    }
    if (ch.res <= 0) ch.res = 1;
    getVolume(ch.unit, ch.channel, ch.cur);
    ch.rangeValid = ch.max >= ch.min;
    return ch.rangeValid;
}

std::vector<VolumeChannel> AudioControl::volumeChannels() {
    std::vector<VolumeChannel> result;
    for (uint8_t id : fn_.playbackFeatureUnits()) {
        const FeatureUnit* fu = fn_.featureUnit(id);
        if (!fu) continue;
        for (size_t c = 0; c < fu->volume.size(); ++c) {
            if (!fu->volume[c]) continue;
            VolumeChannel ch;
            ch.unit = fu->id;
            ch.channel = static_cast<uint8_t>(c);
            ch.hasMute = c < fu->mute.size() && fu->mute[c];
            queryRange(ch);
            result.push_back(ch);
        }
        // Mute-only channels still need to be unmuted.
        for (size_t c = 0; c < fu->mute.size(); ++c) {
            if (!fu->mute[c] || (c < fu->volume.size() && fu->volume[c])) continue;
            VolumeChannel ch;
            ch.unit = fu->id;
            ch.channel = static_cast<uint8_t>(c);
            ch.hasMute = true;
            ch.rangeValid = false;
            result.push_back(ch);
        }
    }
    return result;
}

std::vector<VolumeResult> AudioControl::applyVolume(bool toMax, int16_t target, bool allowBeyondMax) {
    std::vector<VolumeResult> results;
    for (VolumeChannel& ch : volumeChannels()) {
        if (ch.hasMute) setMute(ch.unit, ch.channel, false);
        const FeatureUnit* fu = fn_.featureUnit(ch.unit);
        if (!fu || ch.channel >= fu->volume.size() || !fu->volume[ch.channel]) continue;  // mute-only

        VolumeResult r;
        r.channel = ch;
        int16_t v = target;
        if (ch.rangeValid) {
            if (toMax) v = ch.max;
            else if (!allowBeyondMax) v = std::clamp<int16_t>(target, ch.min, ch.max);
        } else if (toMax) {
            v = 0;  // range unknown: 0 dB is the UAC reference level
        }
        r.requested = v;
        r.ok = setVolume(ch.unit, ch.channel, v);
        r.readBackValid = getVolume(ch.unit, ch.channel, r.readBack);
        results.push_back(r);
    }
    return results;
}

std::vector<ProbeResult> AudioControl::probeAboveMax(int limitDb, bool keepHighest) {
    std::vector<ProbeResult> results;
    for (VolumeChannel& ch : volumeChannels()) {
        if (!ch.rangeValid) continue;
        ProbeResult pr;
        pr.channel = ch;
        int16_t best = ch.max;
        if (setVolume(ch.unit, ch.channel, ch.max)) {
            int16_t rb;
            if (getVolume(ch.unit, ch.channel, rb)) best = rb;
        }
        for (int db = 1; db <= limitDb; ++db) {
            int v = ch.max + db * kDb;
            if (v > INT16_MAX) break;
            if (!setVolume(ch.unit, ch.channel, static_cast<int16_t>(v))) break;
            int16_t rb;
            if (!getVolume(ch.unit, ch.channel, rb)) break;
            if (rb <= best) break;  // device clamps: no point going further
            best = rb;
        }
        pr.highestAccepted = best;
        pr.aboveMax = best > ch.max;
        setVolume(ch.unit, ch.channel, keepHighest ? best : ch.max);
        results.push_back(pr);
    }
    return results;
}

std::vector<uint32_t> AudioControl::querySampleRates(const StreamingAlt& alt) {
    std::vector<uint32_t> rates;
    if (fn_.version != Version::UAC2) return alt.rates;
    const uint8_t clock = fn_.clockSourceFor(alt);
    if (clock == 0) return rates;
    const uint16_t value = kCsSamplingFreq << 8;
    const uint16_t index = unitIndex(clock);
    uint8_t head[2] = {};
    if (in(kRange, value, index, head, 2) < 2) return rates;
    const int n = head[0] | (head[1] << 8);
    if (n <= 0 || n > 64) return rates;
    std::vector<uint8_t> buf(2 + 12 * n);
    int r = in(kRange, value, index, buf.data(), static_cast<uint16_t>(buf.size()));
    if (r < 2 + 12) return rates;
    const int got = std::min(n, (r - 2) / 12);
    for (int i = 0; i < got; ++i) {
        const uint8_t* p = buf.data() + 2 + 12 * i;
        uint32_t lo = u32(p), hi = u32(p + 4), res = u32(p + 8);
        if (lo == hi) {
            rates.push_back(lo);
            continue;
        }
        for (uint32_t s : standardRates())
            if (s >= lo && s <= hi && (res == 0 || (s - lo) % res == 0)) rates.push_back(s);
    }
    std::sort(rates.begin(), rates.end());
    rates.erase(std::unique(rates.begin(), rates.end()), rates.end());
    return rates;
}

bool AudioControl::setSampleRate(const StreamingAlt& alt, uint32_t rate) {
    if (fn_.version == Version::UAC2) {
        const uint8_t clockId = fn_.clockSourceFor(alt);
        const Clock* clock = fn_.clock(clockId);
        if (!clock) return false;
        // Fixed clocks cannot be programmed; the rate is valid if it's the only one.
        if (!clock->frequencyProgrammable()) return true;
        uint8_t buf[4] = {static_cast<uint8_t>(rate), static_cast<uint8_t>(rate >> 8), static_cast<uint8_t>(rate >> 16),
                          static_cast<uint8_t>(rate >> 24)};
        return out(kCur, kCsSamplingFreq << 8, unitIndex(clockId), buf, 4) >= 0;
    }
    uint8_t buf[3] = {static_cast<uint8_t>(rate), static_cast<uint8_t>(rate >> 8), static_cast<uint8_t>(rate >> 16)};
    int r = out(kSetCur, kCsSamplingFreq << 8, alt.data.address, buf, 3, true);
    // Devices with a single fixed rate often stall this request.
    return r >= 0 || !alt.sampleRateControl || alt.rates.size() <= 1;
}

bool AudioControl::getSampleRate(const StreamingAlt& alt, uint32_t& rate) {
    if (fn_.version == Version::UAC2) {
        const uint8_t clockId = fn_.clockSourceFor(alt);
        if (clockId == 0) return false;
        uint8_t buf[4] = {};
        if (in(kCur, kCsSamplingFreq << 8, unitIndex(clockId), buf, 4) < 4) return false;
        rate = u32(buf);
        return true;
    }
    uint8_t buf[3] = {};
    if (in(kGetCur, kCsSamplingFreq << 8, alt.data.address, buf, 3, true) < 3) return false;
    rate = buf[0] | (buf[1] << 8) | (buf[2] << 16);
    return true;
}

}  // namespace uac
