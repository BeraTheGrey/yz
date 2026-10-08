#include "resampler.h"

#include <algorithm>
#include <cmath>

namespace dsp {
namespace {

double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    const double q = x * x / 4.0;
    for (int k = 1; k < 64; ++k) {
        term *= q / (static_cast<double>(k) * k);
        sum += term;
        if (term < sum * 1e-12) break;
    }
    return sum;
}

}  // namespace

Resampler::Resampler(uint32_t inRate, uint32_t outRate, int channels, int halfTaps, int phases)
    : inRate_(inRate), outRate_(outRate), channels_(channels), half_(halfTaps), phases_(phases) {
    stepInt_ = inRate_ / outRate_;
    stepFrac_ = inRate_ % outRate_;

    // Cutoff relative to the input Nyquist; lower it when downsampling.
    const double ratio = std::min(1.0, static_cast<double>(outRate_) / inRate_);
    const double fc = 0.97 * ratio;
    const double beta = 9.0;
    const double i0b = besselI0(beta);
    const int taps = 2 * half_;
    table_.assign(static_cast<size_t>(phases_ + 1) * taps, 0.0f);
    for (int p = 0; p <= phases_; ++p) {
        const double frac = static_cast<double>(p) / phases_;
        double sum = 0.0;
        float* row = &table_[static_cast<size_t>(p) * taps];
        for (int j = 0; j < taps; ++j) {
            const double t = (j - half_ + 1) - frac;  // distance from the output instant
            const double x = fc * t;
            const double sinc = std::fabs(x) < 1e-12 ? 1.0 : std::sin(M_PI * x) / (M_PI * x);
            const double w = std::fabs(t) >= half_ ? 0.0 : besselI0(beta * std::sqrt(1.0 - (t / half_) * (t / half_))) / i0b;
            const double h = fc * sinc * w;
            row[j] = static_cast<float>(h);
            sum += h;
        }
        if (sum != 0.0)
            for (int j = 0; j < taps; ++j) row[j] = static_cast<float>(row[j] / sum);
    }
    reset();
}

void Resampler::reset() {
    // Pre-roll of half_ silent frames so the first output is centred on the
    // first real input frame (constant group delay, no initial transient).
    buf_.assign(static_cast<size_t>(half_) * channels_, 0.0f);
    pos_ = static_cast<size_t>(half_);
    frac_ = 0;
}

void Resampler::process(const float* in, size_t frames, std::vector<float>& out) {
    buf_.insert(buf_.end(), in, in + frames * channels_);
    const size_t avail = buf_.size() / channels_;
    const int taps = 2 * half_;
    const float* data = buf_.data();

    // Output sample at position pos_ + frac uses inputs pos_-half_+1 .. pos_+half_.
    while (pos_ + half_ < avail) {
        const double phase = static_cast<double>(frac_) * phases_ / outRate_;
        const int p0 = static_cast<int>(phase);
        const float mu = static_cast<float>(phase - p0);
        const float* r0 = &table_[static_cast<size_t>(p0) * taps];
        const float* r1 = r0 + taps;
        const float* base = data + (pos_ + 1 - half_) * channels_;
        for (int c = 0; c < channels_; ++c) {
            float acc0 = 0.0f, acc1 = 0.0f;
            for (int j = 0; j < taps; ++j) {
                const float s = base[j * channels_ + c];
                acc0 += s * r0[j];
                acc1 += s * r1[j];
            }
            out.push_back(acc0 + (acc1 - acc0) * mu);
        }
        pos_ += stepInt_;
        frac_ += stepFrac_;
        if (frac_ >= outRate_) {
            frac_ -= outRate_;
            ++pos_;
        }
    }

    // Drop history that is no longer needed.
    const size_t keepFrom = pos_ + 1 >= static_cast<size_t>(half_) ? pos_ + 1 - half_ : 0;
    if (keepFrom > 0) {
        const size_t drop = std::min(keepFrom, avail);
        buf_.erase(buf_.begin(), buf_.begin() + drop * channels_);
        pos_ -= drop;
    }
}

}  // namespace dsp
