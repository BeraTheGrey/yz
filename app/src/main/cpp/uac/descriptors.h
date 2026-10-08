// USB Audio Class (UAC1 / UAC2) descriptor parser.
//
// Pure C++ without libusb/Android dependencies so it can be unit-tested on the host.
// The input is the raw descriptor blob Android returns from
// UsbDeviceConnection.getRawDescriptors() (device descriptor followed by the
// configuration descriptor), but a bare configuration descriptor works too.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace uac {

enum class Version { Unknown, UAC1, UAC2 };

// Terminal types (USB Audio Terminal Types spec).
constexpr uint16_t kTerminalUsbStreaming = 0x0101;

struct Terminal {
    uint8_t id = 0;
    uint16_t type = 0;
    uint8_t sourceId = 0;   // output terminals only
    uint8_t clockId = 0;    // UAC2 only
    bool input = false;
};

// Mixer / selector / processing / extension / effect units. Only the topology
// matters to us, so they are reduced to an id and a list of source ids.
struct Unit {
    uint8_t id = 0;
    uint8_t subtype = 0;
    std::vector<uint8_t> sources;
};

struct FeatureUnit {
    uint8_t id = 0;
    uint8_t sourceId = 0;
    // Index 0 is the master channel, 1..n the logical channels.
    std::vector<bool> volume;  // host-programmable volume control present
    std::vector<bool> mute;    // host-programmable mute control present

    int channelCount() const { return volume.empty() ? 0 : static_cast<int>(volume.size()) - 1; }
    bool hasAnyVolume() const;
};

enum class ClockKind { Source, Selector, Multiplier };

struct Clock {
    uint8_t id = 0;
    ClockKind kind = ClockKind::Source;
    uint8_t attributes = 0;
    uint8_t controls = 0;
    std::vector<uint8_t> sources;  // selector / multiplier inputs

    // UAC2: bmControls bits 1..0 == 0b11 means the frequency is host programmable.
    bool frequencyProgrammable() const { return (controls & 0x03) == 0x03; }
};

struct Endpoint {
    uint8_t address = 0;
    uint8_t attributes = 0;
    uint16_t maxPacketSize = 0;
    uint8_t interval = 0;
    uint8_t synchAddress = 0;

    bool valid() const { return address != 0; }
    bool isIn() const { return (address & 0x80) != 0; }
    bool isIsochronous() const { return (attributes & 0x03) == 0x01; }
    // 1 = asynchronous, 2 = adaptive, 3 = synchronous
    uint8_t syncType() const { return (attributes >> 2) & 0x03; }
    // 0 = data, 1 = explicit feedback, 2 = implicit feedback data
    uint8_t usageType() const { return (attributes >> 4) & 0x03; }
    // Bytes per service interval, including high-bandwidth multiplier.
    int bytesPerInterval(bool highSpeed) const;
};

struct StreamingAlt {
    uint8_t interfaceNumber = 0;
    uint8_t altSetting = 0;
    uint8_t terminalLink = 0;
    bool pcm = false;
    uint8_t channels = 0;
    uint8_t subslotBytes = 0;   // container size of one sample
    uint8_t bitResolution = 0;  // valid bits inside the container (MSB aligned)
    // UAC1: rates from the format descriptor. UAC2: filled from the clock
    // RANGE request at runtime (see AudioControl::querySampleRates).
    std::vector<uint32_t> rates;
    bool continuous = false;
    uint32_t minRate = 0;
    uint32_t maxRate = 0;
    bool sampleRateControl = false;  // UAC1 class-specific endpoint bit
    Endpoint data;
    Endpoint feedback;

    bool isPlayback() const { return data.valid() && !data.isIn(); }
    int frameBytes() const { return channels * subslotBytes; }
    bool supportsRate(uint32_t rate) const;
};

struct AudioFunction {
    Version version = Version::Unknown;
    uint8_t controlInterface = 0;
    uint16_t vendorId = 0;
    uint16_t productId = 0;
    uint16_t bcdDevice = 0;
    std::vector<Terminal> terminals;
    std::vector<Unit> units;
    std::vector<FeatureUnit> featureUnits;
    std::vector<Clock> clocks;
    std::vector<StreamingAlt> alts;

    const Terminal* terminal(uint8_t id) const;
    const FeatureUnit* featureUnit(uint8_t id) const;
    const Unit* unit(uint8_t id) const;
    const Clock* clock(uint8_t id) const;

    // Feature units that sit between a USB-streaming input terminal and a
    // physical output (headphones, speaker, line out). These are the ones that
    // attenuate what we hear.
    std::vector<uint8_t> playbackFeatureUnits() const;

    // UAC2: clock source id feeding the given streaming alt (resolving
    // selectors / multipliers to their first input). 0 when none.
    uint8_t clockSourceFor(const StreamingAlt& alt) const;

    std::vector<const StreamingAlt*> playbackAlts() const;
};

bool parse(const uint8_t* data, size_t length, AudioFunction& out, std::string* error);

// Standard rates used to expand continuous ranges into a discrete list.
const std::vector<uint32_t>& standardRates();

}  // namespace uac
