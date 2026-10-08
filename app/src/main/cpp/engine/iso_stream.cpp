#include "iso_stream.h"

#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#include "log.h"

namespace engine {
namespace {

constexpr int kTransfers = 6;
constexpr int kMsPerTransfer = 4;
constexpr int kFeedbackTransfers = 2;
constexpr int kRingMs = 160;

}  // namespace

IsoStream::IsoStream(libusb_context* ctx, libusb_device_handle* handle, const uac::StreamingAlt& alt, uint32_t rate,
                     bool highSpeed)
    : ctx_(ctx),
      handle_(handle),
      alt_(alt),
      rate_(rate),
      highSpeed_(highSpeed),
      frameBytes_(static_cast<size_t>(std::max(1, alt.frameBytes()))),
      ring_(std::max<size_t>(frameBytes_ * 64,
                             (static_cast<size_t>(rate) * kRingMs / 1000) * static_cast<size_t>(std::max(1, alt.frameBytes())))) {
    const int interval = std::clamp<int>(alt.data.interval, 1, 4);
    if (highSpeed_) {
        microframesPerPacket_ = 1u << (interval - 1);
        packetsPerSecond_ = 8000 / microframesPerPacket_;
    } else {
        microframesPerPacket_ = 1;
        packetsPerSecond_ = 1000u >> (interval - 1);
    }
    packetsPerTransfer_ = std::max<int>(1, static_cast<int>(packetsPerSecond_ * kMsPerTransfer / 1000));
    maxPacketBytes_ = alt.data.bytesPerInterval(highSpeed_);
    clock_ = PacketClock(nominalFramesQ16(rate_, packetsPerSecond_));
}

IsoStream::~IsoStream() { stop(); }

bool IsoStream::start(std::string& error) {
    if (running_) return true;
    if (maxPacketBytes_ < static_cast<int>(frameBytes_)) {
        error = "Uç noktanın paket boyutu bu format için yetersiz";
        return false;
    }
    ring_.clear();
    running_ = true;
    failed_ = false;

    const int dataBufBytes = packetsPerTransfer_ * maxPacketBytes_;
    for (int i = 0; i < kTransfers; ++i) {
        libusb_transfer* t = libusb_alloc_transfer(packetsPerTransfer_);
        if (!t) {
            error = "libusb_alloc_transfer başarısız";
            stop();
            return false;
        }
        buffers_.emplace_back(new uint8_t[dataBufBytes]());
        libusb_fill_iso_transfer(t, handle_, alt_.data.address, buffers_.back().get(), dataBufBytes, packetsPerTransfer_,
                                 &IsoStream::onData, this, 0);
        transfers_.push_back(t);
    }

    const bool explicitFeedback = alt_.feedback.valid();
    if (explicitFeedback) {
        const int fbBytes = std::max(4, alt_.feedback.bytesPerInterval(highSpeed_));
        for (int i = 0; i < kFeedbackTransfers; ++i) {
            libusb_transfer* t = libusb_alloc_transfer(1);
            if (!t) break;
            buffers_.emplace_back(new uint8_t[fbBytes]());
            libusb_fill_iso_transfer(t, handle_, alt_.feedback.address, buffers_.back().get(), fbBytes, 1,
                                     &IsoStream::onFeedback, this, 0);
            libusb_set_iso_packet_lengths(t, static_cast<unsigned>(fbBytes));
            transfers_.push_back(t);
        }
    }

    thread_ = std::thread(&IsoStream::eventLoop, this);

    for (libusb_transfer* t : transfers_) {
        if (t->callback == &IsoStream::onData) fill(t);
        int r = libusb_submit_transfer(t);
        if (r != 0) {
            LOGW("submit failed: %s", libusb_error_name(r));
            if (t->callback == &IsoStream::onData) {
                error = std::string("İzokron aktarım başlatılamadı: ") + libusb_error_name(r);
                stop();
                return false;
            }
            continue;  // feedback is optional; fall back to the nominal rate
        }
        ++active_;
    }
    LOGI("stream started: %u Hz, %zu B/frame, %u pkt/s, %d pkt/xfer, maxPkt %d, feedback %s", rate_, frameBytes_,
         packetsPerSecond_, packetsPerTransfer_, maxPacketBytes_, explicitFeedback ? "yes" : "no");
    return true;
}

void IsoStream::fill(libusb_transfer* t) {
    const uint32_t maxFrames = static_cast<uint32_t>(maxPacketBytes_ / frameBytes_);
    uint8_t* buf = t->buffer;
    int offset = 0;
    uint64_t frames = 0;
    for (int i = 0; i < t->num_iso_packets; ++i) {
        const uint32_t f = clock_.next(maxFrames);
        const size_t bytes = f * frameBytes_;
        const size_t got = ring_.read(buf + offset, bytes);
        if (got < bytes) {
            std::memset(buf + offset + got, 0, bytes - got);
            // Count the moment data runs out (not every silent packet while paused).
            if (got > 0 || lastHadData_) ++underruns_;
        }
        if (bytes > 0) lastHadData_ = got == bytes;
        t->iso_packet_desc[i].length = static_cast<unsigned>(bytes);
        offset += static_cast<int>(bytes);
        frames += f;
    }
    t->length = offset;
    framesSent_ += frames;
    spaceCv_.notify_all();
}

void IsoStream::finished() { --active_; }

void LIBUSB_CALL IsoStream::onData(libusb_transfer* t) {
    auto* self = static_cast<IsoStream*>(t->user_data);
    if (t->status == LIBUSB_TRANSFER_NO_DEVICE) self->failed_ = true;
    if (!self->running_ || t->status == LIBUSB_TRANSFER_CANCELLED || t->status == LIBUSB_TRANSFER_NO_DEVICE) {
        self->finished();
        return;
    }
    if (t->status != LIBUSB_TRANSFER_COMPLETED) ++self->errors_;
    self->fill(t);
    if (libusb_submit_transfer(t) != 0) {
        self->failed_ = true;
        self->finished();
    }
}

void LIBUSB_CALL IsoStream::onFeedback(libusb_transfer* t) {
    auto* self = static_cast<IsoStream*>(t->user_data);
    if (!self->running_ || t->status == LIBUSB_TRANSFER_CANCELLED || t->status == LIBUSB_TRANSFER_NO_DEVICE) {
        self->finished();
        return;
    }
    if (t->status == LIBUSB_TRANSFER_COMPLETED && t->iso_packet_desc[0].status == LIBUSB_TRANSFER_COMPLETED) {
        const int len = static_cast<int>(t->iso_packet_desc[0].actual_length);
        const uint32_t q16 = decodeFeedback(t->buffer, len, self->clock_.nominalQ16(), self->microframesPerPacket_);
        if (q16) {
            self->clock_.setFeedback(q16);
            self->feedback_ = q16;
        }
    }
    if (libusb_submit_transfer(t) != 0) self->finished();
}

void IsoStream::eventLoop() {
    // Same niceness as Android's THREAD_PRIORITY_URGENT_AUDIO.
    setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), -19);
    auto deadline = std::chrono::steady_clock::time_point::max();
    while (running_ || active_ > 0) {
        timeval tv{0, 50000};
        libusb_handle_events_timeout_completed(ctx_, &tv, nullptr);
        if (!running_) {
            const auto now = std::chrono::steady_clock::now();
            if (deadline == std::chrono::steady_clock::time_point::max()) deadline = now + std::chrono::seconds(2);
            else if (now > deadline) {
                LOGW("transfers did not finish after cancel (%d left)", active_.load());
                break;
            }
        }
    }
}

void IsoStream::stop() {
    const bool wasRunning = running_.exchange(false);
    if (wasRunning || thread_.joinable()) {
        for (libusb_transfer* t : transfers_) libusb_cancel_transfer(t);
    }
    if (thread_.joinable()) thread_.join();
    spaceCv_.notify_all();
    if (active_ <= 0) {
        for (libusb_transfer* t : transfers_) libusb_free_transfer(t);
        buffers_.clear();
    } else {
        // A transfer is still owned by the kernel; leaking is safer than a use-after-free.
        for (auto& b : buffers_) b.release();
        buffers_.clear();
    }
    transfers_.clear();
    active_ = 0;
}

size_t IsoStream::write(const uint8_t* data, size_t bytes, int timeoutMs) {
    size_t done = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (done < bytes && running_ && !failed_) {
        size_t space = ring_.space();
        space -= space % frameBytes_;
        if (space == 0) {
            std::unique_lock<std::mutex> lock(mutex_);
            if (spaceCv_.wait_until(lock, std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(20))) ==
                    std::cv_status::timeout &&
                std::chrono::steady_clock::now() >= deadline)
                break;
            continue;
        }
        done += ring_.write(data + done, std::min(bytes - done, space));
    }
    return done;
}

StreamStats IsoStream::stats() const {
    StreamStats s;
    s.framesSent = framesSent_;
    s.underrunPackets = underruns_;
    s.errors = errors_;
    s.feedbackQ16 = feedback_;
    s.nominalQ16 = clock_.nominalQ16();
    s.bufferedFrames = static_cast<uint32_t>(ring_.available() / frameBytes_);
    s.bufferCapacityFrames = static_cast<uint32_t>(ring_.capacity() / frameBytes_);
    s.running = running_ && !failed_;
    return s;
}

}  // namespace engine
