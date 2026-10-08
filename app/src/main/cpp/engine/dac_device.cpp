#include "dac_device.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "log.h"

namespace engine {
namespace {

const char* syncName(uint8_t t) {
    switch (t) {
        case 1: return "async";
        case 2: return "adaptive";
        case 3: return "sync";
        default: return "none";
    }
}

bool sameFamily(uint32_t a, uint32_t b) { return (a % 11025 == 0) == (b % 11025 == 0); }

}  // namespace

std::unique_ptr<DacDevice> DacDevice::open(int fd, const uint8_t* raw, size_t rawLength, std::string& error) {
    std::unique_ptr<DacDevice> d(new DacDevice());
    if (!uac::parse(raw, rawLength, d->fn_, &error)) return nullptr;

    // Android apps cannot enumerate /dev/bus/usb; we only wrap the fd that
    // UsbManager handed out.
    libusb_set_option(nullptr, LIBUSB_OPTION_NO_DEVICE_DISCOVERY, nullptr);
    int r = libusb_init(&d->ctx_);
    if (r != 0) {
        error = std::string("libusb_init: ") + libusb_error_name(r);
        d->ctx_ = nullptr;
        return nullptr;
    }
    libusb_set_option(d->ctx_, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING);
    r = libusb_wrap_sys_device(d->ctx_, static_cast<intptr_t>(fd), &d->handle_);
    if (r != 0 || !d->handle_) {
        error = std::string("libusb_wrap_sys_device: ") + libusb_error_name(r);
        d->handle_ = nullptr;
        return nullptr;
    }
    d->transport_ = std::make_unique<LibusbTransport>(d->handle_);
    d->control_ = std::make_unique<uac::AudioControl>(*d->transport_, d->fn_);
    d->uac2Rates_.resize(d->fn_.alts.size());
    LOGI("opened %04x:%04x UAC%d, control iface %d, %zu alt settings", d->fn_.vendorId, d->fn_.productId,
         d->fn_.version == uac::Version::UAC2 ? 2 : 1, d->fn_.controlInterface, d->fn_.alts.size());
    return d;
}

DacDevice::~DacDevice() {
    stopStream();
    if (handle_) libusb_close(handle_);  // does not close the wrapped fd
    if (ctx_) libusb_exit(ctx_);
}

bool DacDevice::highSpeed() const {
    const int speed = libusb_get_device_speed(libusb_get_device(handle_));
    if (speed == LIBUSB_SPEED_UNKNOWN) return fn_.version == uac::Version::UAC2;
    return speed >= LIBUSB_SPEED_HIGH;
}

uint32_t DacDevice::packetsPerSecond(const uac::StreamingAlt& alt) const {
    const int interval = std::clamp<int>(alt.data.interval, 1, 4);
    return highSpeed() ? 8000u >> (interval - 1) : 1000u >> (interval - 1);
}

std::vector<int> DacDevice::audioInterfaces() const {
    std::set<int> s;
    for (const auto& a : fn_.alts) s.insert(a.interfaceNumber);
    std::vector<int> v(s.begin(), s.end());
    v.push_back(fn_.controlInterface);  // control interface last: re-probing it rebinds everything
    return v;
}

void DacDevice::detachAudioInterfaces() {
    // Detaching only the control interface would leave the streaming
    // interfaces bound to a dead sound card, and the re-probed card would have
    // no PCM devices. So every audio interface is released from the driver.
    for (int iface : audioInterfaces()) {
        int r = libusb_detach_kernel_driver(handle_, iface);
        if (r != 0 && r != LIBUSB_ERROR_NOT_FOUND) LOGW("detach iface %d: %s", iface, libusb_error_name(r));
    }
}

void DacDevice::reattach(ReattachMode mode) {
    if (mode == ReattachMode::UsbReset) {
        int r = libusb_reset_device(handle_);
        LOGI("usb reset: %s", libusb_error_name(r));
        return;
    }
    // Attach the control interface last; snd-usb-audio's probe claims the
    // streaming interfaces itself, so their attach may report BUSY.
    bool controlOk = false;
    for (int iface : audioInterfaces()) {
        int r = libusb_attach_kernel_driver(handle_, iface);
        if (iface == fn_.controlInterface) controlOk = r == 0 || r == LIBUSB_ERROR_BUSY;
        if (r != 0 && r != LIBUSB_ERROR_BUSY && r != LIBUSB_ERROR_NOT_FOUND)
            LOGW("attach iface %d: %s", iface, libusb_error_name(r));
    }
    if (!controlOk) {
        LOGW("kernel driver re-attach failed, falling back to USB reset");
        libusb_reset_device(handle_);
    }
}

bool DacDevice::claimControl(std::string& error) {
    if (exclusive_) return true;
    detachAudioInterfaces();
    int r = libusb_claim_interface(handle_, fn_.controlInterface);
    if (r != 0) {
        error = std::string("Kontrol arayüzü alınamadı: ") + libusb_error_name(r);
        reattach(ReattachMode::AttachDriver);
        return false;
    }
    // Cache UAC2 clock rates while we hold the interface.
    if (fn_.version == uac::Version::UAC2)
        for (size_t i = 0; i < fn_.alts.size(); ++i)
            if (uac2Rates_[i].empty() && fn_.alts[i].isPlayback()) uac2Rates_[i] = control_->querySampleRates(fn_.alts[i]);
    return true;
}

void DacDevice::releaseControl(ReattachMode mode) {
    if (exclusive_) return;
    libusb_release_interface(handle_, fn_.controlInterface);
    reattach(mode);
}

void DacDevice::writeChannels(Json& j, const std::vector<uac::VolumeChannel>& chans) {
    j.beginArray();
    for (const auto& c : chans) {
        j.beginObject()
            .field("unit", static_cast<int>(c.unit))
            .field("channel", static_cast<int>(c.channel))
            .field("rangeValid", c.rangeValid)
            .field("min", static_cast<int>(c.min))
            .field("max", static_cast<int>(c.max))
            .field("res", static_cast<int>(c.res))
            .field("cur", static_cast<int>(c.cur))
            .field("hasMute", c.hasMute)
            .endObject();
    }
    j.endArray();
}

std::vector<uint32_t> DacDevice::ratesFor(const uac::StreamingAlt& alt) {
    if (fn_.version != uac::Version::UAC2) return alt.rates;
    for (size_t i = 0; i < fn_.alts.size(); ++i)
        if (&fn_.alts[i] == &alt && !uac2Rates_[i].empty()) return uac2Rates_[i];
    return {};
}

std::string DacDevice::describeJson() {
    Json j;
    j.beginObject()
        .field("vendorId", static_cast<int>(fn_.vendorId))
        .field("productId", static_cast<int>(fn_.productId))
        .field("bcdDevice", static_cast<int>(fn_.bcdDevice))
        .field("uacVersion", fn_.version == uac::Version::UAC2 ? 2 : 1)
        .field("highSpeed", highSpeed())
        .field("controlInterface", static_cast<int>(fn_.controlInterface))
        .field("exclusive", exclusive_);
    j.key("featureUnits").beginArray();
    for (uint8_t id : fn_.playbackFeatureUnits()) j.value(static_cast<int>(id));
    j.endArray();
    j.key("alts").beginArray();
    for (size_t i = 0; i < fn_.alts.size(); ++i) {
        const auto& a = fn_.alts[i];
        j.beginObject()
            .field("interface", static_cast<int>(a.interfaceNumber))
            .field("alt", static_cast<int>(a.altSetting))
            .field("playback", a.isPlayback())
            .field("pcm", a.pcm)
            .field("channels", static_cast<int>(a.channels))
            .field("subslot", static_cast<int>(a.subslotBytes))
            .field("bits", static_cast<int>(a.bitResolution))
            .field("sync", syncName(a.data.syncType()))
            .field("feedback", a.feedback.valid())
            .field("maxPacket", a.data.bytesPerInterval(highSpeed()));
        j.key("rates").beginArray();
        for (uint32_t r : ratesFor(a)) j.value(static_cast<unsigned>(r));
        j.endArray();
        j.endObject();
    }
    j.endArray();
    j.key("volume");
    writeChannels(j, lastChannels_);
    j.endObject();
    return j.str();
}

std::string DacDevice::volumeJson(ReattachMode mode) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    Json j;
    std::string err;
    j.beginObject();
    if (!claimControl(err)) {
        j.field("ok", false).field("error", err).endObject();
        return j.str();
    }
    lastChannels_ = control_->volumeChannels();
    releaseControl(mode);
    j.field("ok", true).key("channels");
    writeChannels(j, lastChannels_);
    j.endObject();
    return j.str();
}

std::string DacDevice::applyVolumeJson(bool toMax, int16_t target, bool allowBeyondMax, ReattachMode mode) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    Json j;
    std::string err;
    j.beginObject();
    if (!claimControl(err)) {
        j.field("ok", false).field("error", err).endObject();
        return j.str();
    }
    auto results = control_->applyVolume(toMax, target, allowBeyondMax);
    lastChannels_ = control_->volumeChannels();
    releaseControl(mode);

    bool allOk = !results.empty();
    j.key("results").beginArray();
    for (const auto& r : results) {
        allOk = allOk && r.ok;
        j.beginObject()
            .field("unit", static_cast<int>(r.channel.unit))
            .field("channel", static_cast<int>(r.channel.channel))
            .field("min", static_cast<int>(r.channel.min))
            .field("max", static_cast<int>(r.channel.max))
            .field("requested", static_cast<int>(r.requested))
            .field("ok", r.ok)
            .field("readBackValid", r.readBackValid)
            .field("readBack", static_cast<int>(r.readBack))
            .endObject();
    }
    j.endArray();
    j.field("ok", allOk);
    if (results.empty()) j.field("error", "Cihazda ayarlanabilir donanım ses kontrolü bulunamadı");
    j.endObject();
    return j.str();
}

std::string DacDevice::probeAboveMaxJson(int limitDb, bool keepHighest, ReattachMode mode) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    Json j;
    std::string err;
    j.beginObject();
    if (!claimControl(err)) {
        j.field("ok", false).field("error", err).endObject();
        return j.str();
    }
    auto results = control_->probeAboveMax(limitDb, keepHighest);
    lastChannels_ = control_->volumeChannels();
    releaseControl(mode);
    j.field("ok", !results.empty()).key("results").beginArray();
    for (const auto& r : results) {
        j.beginObject()
            .field("unit", static_cast<int>(r.channel.unit))
            .field("channel", static_cast<int>(r.channel.channel))
            .field("max", static_cast<int>(r.channel.max))
            .field("highestAccepted", static_cast<int>(r.highestAccepted))
            .field("aboveMax", r.aboveMax)
            .endObject();
    }
    j.endArray().endObject();
    return j.str();
}

const uac::StreamingAlt* DacDevice::chooseAlt(uint32_t sourceRate, int sourceChannels, uint32_t forcedRate,
                                              uint32_t& rateOut) {
    const auto alts = fn_.playbackAlts();
    if (alts.empty()) return nullptr;

    auto rates = [&](const uac::StreamingAlt* a) {
        auto r = ratesFor(*a);
        if (r.empty()) r = {44100, 48000};  // UAC2 clock did not answer RANGE
        return r;
    };
    auto fits = [&](const uac::StreamingAlt* a, uint32_t rate) {
        return a->fitsRate(rate, packetsPerSecond(*a), highSpeed());
    };
    auto supports = [&](const uac::StreamingAlt* a, uint32_t rate) {
        auto r = rates(a);
        return (std::find(r.begin(), r.end(), rate) != r.end() || (a->continuous && a->supportsRate(rate))) &&
               fits(a, rate);
    };

    std::set<uint32_t> all;
    for (auto* a : alts)
        for (uint32_t r : rates(a))
            if (fits(a, r)) all.insert(r);
    if (all.empty()) return nullptr;

    uint32_t target = 0;
    if (forcedRate && all.count(forcedRate)) target = forcedRate;
    else if (all.count(sourceRate)) target = sourceRate;
    else {
        for (uint32_t r : all)
            if (sameFamily(r, sourceRate) && r >= sourceRate) { target = r; break; }
        if (!target)
            for (uint32_t r : all)
                if (sameFamily(r, sourceRate)) target = r;  // highest of the family
        if (!target)
            for (uint32_t r : all)
                if (r >= sourceRate) { target = r; break; }
        if (!target) target = *all.rbegin();
    }

    const uac::StreamingAlt* best = nullptr;
    auto score = [&](const uac::StreamingAlt* a) {
        int s = a->bitResolution * 100;
        if (a->channels == sourceChannels) s += 20;
        else if (a->channels == 2) s += 10;
        s -= a->subslotBytes;  // prefer the tighter container at equal resolution
        return s;
    };
    for (auto* a : alts)
        if (supports(a, target) && (!best || score(a) > score(best))) best = a;
    rateOut = target;
    return best;
}

bool DacDevice::streaming() {
    std::lock_guard<std::mutex> lock(streamMutex_);
    return stream_ != nullptr;
}

std::string DacDevice::startStream(uint32_t sourceRate, int sourceChannels, int sourceBits, uint32_t forcedRate) {
    std::lock_guard<std::mutex> dspLock(dspMutex_);
    std::lock_guard<std::mutex> lock(controlMutex_);
    Json j;
    j.beginObject();
    auto fail = [&](const std::string& e) {
        j.field("ok", false).field("error", e).endObject();
        return j.str();
    };

    if (!exclusive_) {
        std::string err;
        if (!claimControl(err)) return fail(err);
        exclusive_ = true;
        claimed_ = {fn_.controlInterface};
        lastChannels_ = control_->volumeChannels();
    }

    uint32_t rate = 0;
    const uac::StreamingAlt* alt = chooseAlt(sourceRate, sourceChannels, forcedRate, rate);
    if (!alt) {
        stopStreamLocked();
        return fail("Uygun çalma formatı bulunamadı");
    }

    std::shared_ptr<IsoStream> current;
    {
        std::lock_guard<std::mutex> sl(streamMutex_);
        current = stream_;
    }
    const bool reuse = current && streamAlt_ == alt && current->rate() == rate && !current->failed();
    if (!reuse) {
        if (current) {
            {
                std::lock_guard<std::mutex> sl(streamMutex_);
                stream_.reset();
            }
            current->stop();
            current.reset();
        }
        if (std::find(claimed_.begin(), claimed_.end(), alt->interfaceNumber) == claimed_.end()) {
            int r = libusb_claim_interface(handle_, alt->interfaceNumber);
            if (r != 0) {
                stopStreamLocked();
                return fail(std::string("Akış arayüzü alınamadı: ") + libusb_error_name(r));
            }
            claimed_.push_back(alt->interfaceNumber);
        }
        if (fn_.version == uac::Version::UAC2) control_->setSampleRate(*alt, rate);
        int r = libusb_set_interface_alt_setting(handle_, alt->interfaceNumber, alt->altSetting);
        if (r != 0) {
            stopStreamLocked();
            return fail(std::string("Alt ayar seçilemedi: ") + libusb_error_name(r));
        }
        if (fn_.version != uac::Version::UAC2 && !control_->setSampleRate(*alt, rate)) {
            stopStreamLocked();
            return fail("Örnekleme hızı ayarlanamadı");
        }
        auto s = std::make_shared<IsoStream>(ctx_, handle_, *alt, rate, highSpeed());
        std::string err;
        if (!s->start(err)) {
            stopStreamLocked();
            return fail(err);
        }
        std::lock_guard<std::mutex> sl(streamMutex_);
        stream_ = s;
        streamAlt_ = alt;
    }

    uint32_t actualRate = 0;
    const bool rateRead = control_->getSampleRate(*alt, actualRate);

    sourceBits_ = sourceBits;
    quantizer_ = dsp::Quantizer({alt->channels, alt->subslotBytes, alt->bitResolution}, sourceBits);
    resampler_.reset();
    if (sourceRate != rate) resampler_ = std::make_unique<dsp::Resampler>(sourceRate, rate, sourceChannels);

    const bool bitPerfect = sourceRate == rate && sourceBits <= alt->bitResolution && sourceChannels == alt->channels &&
                            gain_.load() == 1.0f && !softClip_.load();
    j.field("ok", true)
        .field("reused", reuse)
        .field("sourceRate", static_cast<unsigned>(sourceRate))
        .field("deviceRate", static_cast<unsigned>(rate))
        .field("deviceRateReadBack", static_cast<unsigned>(rateRead ? actualRate : 0))
        .field("sourceBits", sourceBits)
        .field("deviceBits", static_cast<int>(alt->bitResolution))
        .field("subslot", static_cast<int>(alt->subslotBytes))
        .field("channels", static_cast<int>(alt->channels))
        .field("interface", static_cast<int>(alt->interfaceNumber))
        .field("alt", static_cast<int>(alt->altSetting))
        .field("sync", syncName(alt->data.syncType()))
        .field("feedback", alt->feedback.valid())
        .field("resampling", resampler_ != nullptr)
        .field("dither", quantizer_.dithering())
        .field("bitPerfect", bitPerfect)
        .endObject();
    return j.str();
}

int DacDevice::write(const uint8_t* data, size_t bytes, dsp::InputEncoding enc, int channels, uint32_t rate) {
    std::lock_guard<std::mutex> dspLock(dspMutex_);
    std::shared_ptr<IsoStream> s;
    {
        std::lock_guard<std::mutex> sl(streamMutex_);
        s = stream_;
    }
    if (!s || s->failed() || channels <= 0) return -1;

    const size_t samples = bytes / dsp::bytesPerSample(enc);
    const size_t frames = samples / channels;
    if (frames == 0) return 0;
    floatBuf_.resize(frames * channels);
    dsp::toFloat(data, frames * channels, enc, floatBuf_.data());

    const uint32_t deviceRate = s->rate();
    if (rate != deviceRate) {
        if (!resampler_ || resampler_->inRate() != rate || resampler_->channels() != channels)
            resampler_ = std::make_unique<dsp::Resampler>(rate, deviceRate, channels);
    } else {
        resampler_.reset();
    }

    float* src = floatBuf_.data();
    size_t outFrames = frames;
    if (resampler_) {
        resampled_.clear();
        resampler_->process(floatBuf_.data(), frames, resampled_);
        src = resampled_.data();
        outFrames = resampled_.size() / channels;
    }
    dsp::applyGain(src, outFrames * channels, gain_.load(), softClip_.load());

    outBuf_.resize(outFrames * quantizer_.frameBytes());
    quantizer_.process(src, outFrames, channels, outBuf_.data());
    if (outBuf_.empty()) return static_cast<int>(frames);
    const size_t written = s->write(outBuf_.data(), outBuf_.size(), 3000);
    if (s->failed()) return -1;
    if (written < outBuf_.size()) return written == 0 ? 0 : static_cast<int>(frames);
    return static_cast<int>(frames);
}

void DacDevice::flush() {
    std::shared_ptr<IsoStream> s;
    {
        std::lock_guard<std::mutex> sl(streamMutex_);
        s = stream_;
    }
    if (s) s->flush();
    std::lock_guard<std::mutex> dspLock(dspMutex_);
    if (resampler_) resampler_->reset();
}

void DacDevice::stopStreamLocked() {
    std::shared_ptr<IsoStream> s;
    {
        std::lock_guard<std::mutex> sl(streamMutex_);
        s = stream_;
        stream_.reset();
        streamAlt_ = nullptr;
    }
    if (s) s->stop();
    if (!exclusive_) return;
    for (int iface : claimed_) {
        if (iface != fn_.controlInterface) libusb_set_interface_alt_setting(handle_, iface, 0);
        libusb_release_interface(handle_, iface);
    }
    claimed_.clear();
    exclusive_ = false;
    reattach(ReattachMode::AttachDriver);
}

void DacDevice::stopStream() {
    std::lock_guard<std::mutex> lock(controlMutex_);
    stopStreamLocked();
}

std::string DacDevice::statsJson() {
    std::shared_ptr<IsoStream> s;
    {
        std::lock_guard<std::mutex> sl(streamMutex_);
        s = stream_;
    }
    Json j;
    j.beginObject();
    if (!s) {
        j.field("running", false).endObject();
        return j.str();
    }
    const StreamStats st = s->stats();
    j.field("running", st.running)
        .field("failed", s->failed())
        .field("rate", static_cast<unsigned>(s->rate()))
        .field("framesSent", st.framesSent)
        .field("underruns", st.underrunPackets)
        .field("errors", st.errors)
        .field("feedbackFramesPerPacket", st.feedbackQ16 / 65536.0)
        .field("nominalFramesPerPacket", st.nominalQ16 / 65536.0)
        .field("bufferedFrames", static_cast<unsigned>(st.bufferedFrames))
        .field("bufferCapacityFrames", static_cast<unsigned>(st.bufferCapacityFrames))
        .field("gain", static_cast<double>(gain_.load()))
        .field("softClip", softClip_.load())
        .endObject();
    return j.str();
}

void DacDevice::setDigitalGain(float linear, bool softClip) {
    gain_ = std::isfinite(linear) ? std::clamp(linear, 0.0f, 8.0f) : 1.0f;
    softClip_ = softClip;
}

}  // namespace engine
