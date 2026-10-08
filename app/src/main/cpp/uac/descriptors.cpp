#include "descriptors.h"

#include <algorithm>

namespace uac {
namespace {

constexpr uint8_t kDtDevice = 0x01;
constexpr uint8_t kDtConfig = 0x02;
constexpr uint8_t kDtInterface = 0x04;
constexpr uint8_t kDtEndpoint = 0x05;
constexpr uint8_t kDtCsInterface = 0x24;
constexpr uint8_t kDtCsEndpoint = 0x25;

constexpr uint8_t kClassAudio = 0x01;
constexpr uint8_t kSubclassControl = 0x01;
constexpr uint8_t kSubclassStreaming = 0x02;
constexpr uint8_t kProtocolUac2 = 0x20;
constexpr uint8_t kProtocolUac3 = 0x30;

// AudioControl subtypes (shared numbering in UAC1 and UAC2 up to 0x06).
constexpr uint8_t kAcHeader = 0x01;
constexpr uint8_t kAcInputTerminal = 0x02;
constexpr uint8_t kAcOutputTerminal = 0x03;
constexpr uint8_t kAcMixerUnit = 0x04;
constexpr uint8_t kAcSelectorUnit = 0x05;
constexpr uint8_t kAcFeatureUnit = 0x06;
// UAC1: 0x07 processing, 0x08 extension.
// UAC2: 0x07 effect, 0x08 processing, 0x09 extension, 0x0A-0x0C clocks.
constexpr uint8_t kAc2ClockSource = 0x0A;
constexpr uint8_t kAc2ClockSelector = 0x0B;
constexpr uint8_t kAc2ClockMultiplier = 0x0C;

constexpr uint8_t kAsGeneral = 0x01;
constexpr uint8_t kAsFormatType = 0x02;
constexpr uint8_t kFormatTypeI = 0x01;

uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t le24(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16); }
uint32_t le32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint32_t leN(const uint8_t* p, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n && i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
    return v;
}

void addSources(Unit& u, const uint8_t* p, uint8_t len, int countOffset) {
    if (countOffset >= len) return;
    int n = p[countOffset];
    for (int i = 0; i < n && countOffset + 1 + i < len; ++i) u.sources.push_back(p[countOffset + 1 + i]);
}

void parseControl(const uint8_t* p, uint8_t len, Version v, AudioFunction& f) {
    if (len < 4) return;
    const uint8_t subtype = p[2];
    switch (subtype) {
        case kAcHeader:
            if (len >= 5 && le16(p + 3) >= 0x0200 && v == Version::UAC1) f.version = Version::UAC2;
            break;
        case kAcInputTerminal: {
            if (len < 8) return;
            Terminal t;
            t.id = p[3];
            t.type = le16(p + 4);
            t.input = true;
            if (v == Version::UAC2) t.clockId = p[7];
            f.terminals.push_back(t);
            break;
        }
        case kAcOutputTerminal: {
            if (len < 9) return;
            Terminal t;
            t.id = p[3];
            t.type = le16(p + 4);
            t.sourceId = p[7];
            if (v == Version::UAC2 && len >= 9) t.clockId = p[8];
            f.terminals.push_back(t);
            break;
        }
        case kAcMixerUnit:
        case kAcSelectorUnit: {
            if (len < 5) return;
            Unit u;
            u.id = p[3];
            u.subtype = subtype;
            addSources(u, p, len, 4);
            f.units.push_back(u);
            break;
        }
        case kAcFeatureUnit: {
            FeatureUnit fu;
            if (v == Version::UAC2) {
                if (len < 6) return;
                fu.id = p[3];
                fu.sourceId = p[4];
                int count = (len - 6) / 4;
                for (int i = 0; i < count; ++i) {
                    uint32_t c = le32(p + 5 + 4 * i);
                    fu.mute.push_back((c & 0x03) == 0x03);
                    fu.volume.push_back(((c >> 2) & 0x03) == 0x03);
                }
            } else {
                if (len < 7) return;
                fu.id = p[3];
                fu.sourceId = p[4];
                int size = p[5];
                if (size == 0) return;
                int count = (len - 7) / size;
                for (int i = 0; i < count; ++i) {
                    uint32_t c = leN(p + 6 + size * i, size);
                    fu.mute.push_back((c & 0x01) != 0);
                    fu.volume.push_back((c & 0x02) != 0);
                }
            }
            f.featureUnits.push_back(fu);
            break;
        }
        default:
            if (v == Version::UAC2 && subtype == kAc2ClockSource) {
                if (len < 8) return;
                Clock c;
                c.id = p[3];
                c.kind = ClockKind::Source;
                c.attributes = p[4];
                c.controls = p[5];
                f.clocks.push_back(c);
            } else if (v == Version::UAC2 && subtype == kAc2ClockSelector) {
                if (len < 5) return;
                Clock c;
                c.id = p[3];
                c.kind = ClockKind::Selector;
                for (int i = 0; i < p[4] && 5 + i < len; ++i) c.sources.push_back(p[5 + i]);
                f.clocks.push_back(c);
            } else if (v == Version::UAC2 && subtype == kAc2ClockMultiplier) {
                if (len < 5) return;
                Clock c;
                c.id = p[3];
                c.kind = ClockKind::Multiplier;
                c.sources.push_back(p[4]);
                f.clocks.push_back(c);
            } else if (v == Version::UAC2 && subtype == 0x07) {  // effect unit
                if (len < 7) return;
                Unit u;
                u.id = p[3];
                u.subtype = subtype;
                u.sources.push_back(p[6]);
                f.units.push_back(u);
            } else if ((v == Version::UAC1 && (subtype == 0x07 || subtype == 0x08)) ||
                       (v == Version::UAC2 && (subtype == 0x08 || subtype == 0x09))) {
                // processing / extension units: id, w*Type, bNrInPins, sources...
                if (len < 7) return;
                Unit u;
                u.id = p[3];
                u.subtype = subtype;
                addSources(u, p, len, 6);
                f.units.push_back(u);
            }
            break;
    }
}

void parseStreaming(const uint8_t* p, uint8_t len, Version v, StreamingAlt& a) {
    if (len < 4) return;
    const uint8_t subtype = p[2];
    if (subtype == kAsGeneral) {
        if (v == Version::UAC2) {
            if (len < 11) return;
            a.terminalLink = p[3];
            a.pcm = p[5] == kFormatTypeI && (le32(p + 6) & 0x01);
            a.channels = p[10];
        } else {
            if (len < 7) return;
            a.terminalLink = p[3];
            a.pcm = le16(p + 5) == 0x0001;
        }
    } else if (subtype == kAsFormatType) {
        if (p[3] != kFormatTypeI) {
            a.pcm = false;
            return;
        }
        if (v == Version::UAC2) {
            if (len < 6) return;
            a.subslotBytes = p[4];
            a.bitResolution = p[5];
        } else {
            if (len < 8) return;
            a.channels = p[4];
            a.subslotBytes = p[5];
            a.bitResolution = p[6];
            int n = p[7];
            if (n == 0) {
                if (len < 14) return;
                a.continuous = true;
                a.minRate = le24(p + 8);
                a.maxRate = le24(p + 11);
                for (uint32_t r : standardRates())
                    if (r >= a.minRate && r <= a.maxRate) a.rates.push_back(r);
            } else {
                for (int i = 0; i < n && 8 + 3 * i + 3 <= len; ++i) a.rates.push_back(le24(p + 8 + 3 * i));
                if (!a.rates.empty()) {
                    a.minRate = *std::min_element(a.rates.begin(), a.rates.end());
                    a.maxRate = *std::max_element(a.rates.begin(), a.rates.end());
                }
            }
        }
    }
}

}  // namespace

bool FeatureUnit::hasAnyVolume() const {
    return std::any_of(volume.begin(), volume.end(), [](bool b) { return b; });
}

int Endpoint::bytesPerInterval(bool highSpeed) const {
    int size = maxPacketSize & 0x7FF;
    if (highSpeed) size *= 1 + ((maxPacketSize >> 11) & 0x03);
    return size;
}

bool StreamingAlt::supportsRate(uint32_t rate) const {
    if (continuous && rate >= minRate && rate <= maxRate) return true;
    return std::find(rates.begin(), rates.end(), rate) != rates.end();
}

const Terminal* AudioFunction::terminal(uint8_t id) const {
    for (const auto& t : terminals)
        if (t.id == id) return &t;
    return nullptr;
}

const FeatureUnit* AudioFunction::featureUnit(uint8_t id) const {
    for (const auto& u : featureUnits)
        if (u.id == id) return &u;
    return nullptr;
}

const Unit* AudioFunction::unit(uint8_t id) const {
    for (const auto& u : units)
        if (u.id == id) return &u;
    return nullptr;
}

const Clock* AudioFunction::clock(uint8_t id) const {
    for (const auto& c : clocks)
        if (c.id == id) return &c;
    return nullptr;
}

std::vector<uint8_t> AudioFunction::playbackFeatureUnits() const {
    std::vector<uint8_t> result;
    for (const auto& out : terminals) {
        if (out.input || out.type == kTerminalUsbStreaming) continue;
        // Walk upstream until an input terminal is reached.
        std::vector<uint8_t> chain;
        uint8_t node = out.sourceId;
        bool fromUsb = false;
        for (int depth = 0; depth < 32 && node != 0; ++depth) {
            if (const FeatureUnit* fu = featureUnit(node)) {
                if (fu->hasAnyVolume()) chain.push_back(fu->id);
                node = fu->sourceId;
            } else if (const Unit* u = unit(node)) {
                node = u->sources.empty() ? 0 : u->sources.front();
            } else {
                const Terminal* t = terminal(node);
                fromUsb = t && t->input && t->type == kTerminalUsbStreaming;
                break;
            }
        }
        if (!fromUsb) continue;
        for (uint8_t id : chain)
            if (std::find(result.begin(), result.end(), id) == result.end()) result.push_back(id);
    }
    if (result.empty()) {
        // Unusual topology: fall back to every feature unit that offers volume
        // and is not on a recording path.
        for (const auto& fu : featureUnits) {
            if (!fu.hasAnyVolume()) continue;
            const Terminal* src = terminal(fu.sourceId);
            if (src && src->input && src->type != kTerminalUsbStreaming) continue;
            result.push_back(fu.id);
        }
    }
    return result;
}

uint8_t AudioFunction::clockSourceFor(const StreamingAlt& alt) const {
    const Terminal* t = terminal(alt.terminalLink);
    if (!t) return 0;
    uint8_t id = t->clockId;
    for (int depth = 0; depth < 8 && id != 0; ++depth) {
        const Clock* c = clock(id);
        if (!c) return 0;
        if (c->kind == ClockKind::Source) return c->id;
        id = c->sources.empty() ? 0 : c->sources.front();
    }
    return 0;
}

std::vector<const StreamingAlt*> AudioFunction::playbackAlts() const {
    std::vector<const StreamingAlt*> r;
    for (const auto& a : alts)
        if (a.isPlayback() && a.pcm && a.channels > 0 && a.subslotBytes > 0) r.push_back(&a);
    return r;
}

const std::vector<uint32_t>& standardRates() {
    static const std::vector<uint32_t> rates = {8000,   11025,  16000,  22050,  32000,
                                                44100,  48000,  88200,  96000,  176400,
                                                192000, 352800, 384000, 705600, 768000};
    return rates;
}

bool parse(const uint8_t* d, size_t n, AudioFunction& f, std::string* error) {
    f = AudioFunction();
    int configs = 0;
    int curIface = -1, curClass = -1, curSub = -1;
    int acIface = -1;
    int stream = -1;  // index into f.alts for the current AS alt setting

    for (size_t i = 0; i + 2 <= n;) {
        const uint8_t len = d[i];
        const uint8_t type = d[i + 1];
        if (len < 2 || i + len > n) break;
        const uint8_t* p = d + i;
        i += len;

        if (type == kDtDevice) {
            if (len >= 14) {
                f.vendorId = le16(p + 8);
                f.productId = le16(p + 10);
                f.bcdDevice = le16(p + 12);
            }
        } else if (type == kDtConfig) {
            if (++configs > 1) break;  // only the first configuration is used
        } else if (type == kDtInterface) {
            if (len < 9) continue;
            curIface = p[2];
            curClass = p[5];
            curSub = p[6];
            stream = -1;
            if (curClass != kClassAudio) continue;
            if (curSub == kSubclassControl && acIface < 0) {
                acIface = curIface;
                f.controlInterface = static_cast<uint8_t>(curIface);
                f.version = p[7] == kProtocolUac2   ? Version::UAC2
                            : p[7] == kProtocolUac3 ? Version::Unknown
                                                    : Version::UAC1;
            } else if (curSub == kSubclassStreaming && p[3] != 0) {
                StreamingAlt a;
                a.interfaceNumber = p[2];
                a.altSetting = p[3];
                f.alts.push_back(a);
                stream = static_cast<int>(f.alts.size()) - 1;
            }
        } else if (type == kDtCsInterface) {
            if (curClass != kClassAudio) continue;
            if (curSub == kSubclassControl && curIface == acIface) {
                parseControl(p, len, f.version, f);
            } else if (curSub == kSubclassStreaming && stream >= 0) {
                parseStreaming(p, len, f.version, f.alts[stream]);
            }
        } else if (type == kDtEndpoint) {
            if (stream < 0 || len < 7) continue;
            Endpoint ep;
            ep.address = p[2];
            ep.attributes = p[3];
            ep.maxPacketSize = le16(p + 4);
            ep.interval = p[6];
            if (len >= 9) ep.synchAddress = p[8];
            if (!ep.isIsochronous()) continue;
            StreamingAlt& a = f.alts[stream];
            if (!a.data.valid() && ep.usageType() != 1) {
                a.data = ep;
            } else if (a.data.valid() && ep.isIn() && !a.feedback.valid() &&
                       (ep.usageType() == 1 || ep.address == a.data.synchAddress)) {
                a.feedback = ep;
            }
        } else if (type == kDtCsEndpoint) {
            if (stream < 0 || len < 4) continue;
            if (p[2] == 0x01 && f.version == Version::UAC1) f.alts[stream].sampleRateControl = (p[3] & 0x01) != 0;
        }
    }

    if (acIface < 0) {
        if (error) *error = "USB Audio Control arayüzü bulunamadı";
        return false;
    }
    if (f.version == Version::Unknown) {
        if (error) *error = "UAC3 / bilinmeyen ses sınıfı sürümü desteklenmiyor";
        return false;
    }
    return true;
}

}  // namespace uac
