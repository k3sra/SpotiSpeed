// High-quality variable-rate resampler.
// Windowed-sinc (Kaiser) polyphase-free direct convolution with an adaptive
// cutoff so that speeding up (decimation) stays alias-free.
#pragma once
#include <vector>
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

class Resampler {
public:
    static const int TAPS = 32;          // 16 either side
    static const int HALF = TAPS / 2;

    void configure(int channels) {
        if (ch_ == channels) return;
        ch_ = channels;
        src_.clear();
        pos_ = (double)HALF;
        src_.assign((size_t)HALF * ch_, 0.0f);   // priming history = silence
    }

    void push(const float* interleaved, size_t frames) {
        src_.insert(src_.end(), interleaved, interleaved + frames * (size_t)ch_);
    }

    // frames of source currently available *ahead* of the read cursor
    double pending() const {
        double total = (double)(src_.size() / (size_t)ch_);
        return std::max(0.0, total - pos_ - (double)HALF);
    }

    void reset() {
        src_.assign((size_t)HALF * ch_, 0.0f);
        pos_ = (double)HALF;
    }

    // At ratio 1.0 with an integral read cursor the sinc kernel collapses to an
    // identity (sinc(k)=0 for every non-zero integer k), so 1.0x is bit-exact.
    // Snapping here restores that after returning from a fractional rate.
    void snap() { pos_ = std::floor(pos_ + 0.5); }

    // Produce up to maxFrames output frames at the given rate ratio
    // (ratio = source frames consumed per output frame). Returns frames written.
    size_t process(float* out, size_t maxFrames, double ratio) {
        if (ch_ <= 0) return 0;
        const double cutoff = (ratio > 1.0) ? (1.0 / ratio) : 1.0;  // anti-alias when speeding up
        size_t produced = 0;
        const size_t totalFrames = src_.size() / (size_t)ch_;

        while (produced < maxFrames) {
            // need HALF frames of lookahead past the cursor
            if ((size_t)(pos_ + HALF + 2.0) >= totalFrames) break;

            const long  base = (long)std::floor(pos_);
            const double frac = pos_ - (double)base;

            for (int c = 0; c < ch_; ++c) {
                double acc = 0.0, wsum = 0.0;
                for (int k = -HALF + 1; k <= HALF; ++k) {
                    const long  idx = base + k;
                    if (idx < 0 || (size_t)idx >= totalFrames) continue;
                    const double x = (double)k - frac;
                    const double w = kaiser((x) / (double)HALF) * sincf(cutoff * x) * cutoff;
                    acc  += w * (double)src_[(size_t)idx * (size_t)ch_ + (size_t)c];
                    wsum += w;
                }
                if (wsum > 1e-9) acc /= wsum;              // normalise: unity gain, no level loss
                out[produced * (size_t)ch_ + (size_t)c] =
                    (float)std::max(-1.0, std::min(1.0, acc));
            }
            pos_ += ratio;
            ++produced;
        }
        trim();
        return produced;
    }

private:
    static double sincf(double x) {
        if (std::fabs(x) < 1e-9) return 1.0;
        const double px = M_PI * x;
        return std::sin(px) / px;
    }
    // Kaiser window (beta = 8.6 -> ~90 dB stopband)
    static double kaiser(double t) {
        if (t < -1.0 || t > 1.0) return 0.0;
        const double beta = 8.6;
        return besselI0(beta * std::sqrt(std::max(0.0, 1.0 - t * t))) / besselI0(beta);
    }
    static double besselI0(double x) {
        double s = 1.0, t = 1.0;
        for (int i = 1; i < 24; ++i) {
            t *= (x / (2.0 * i));
            s += t * t;
        }
        return s;
    }
    // drop consumed history, keeping TAPS worth of context
    void trim() {
        const long keepFrom = (long)std::floor(pos_) - HALF;
        if (keepFrom <= 0) return;
        const size_t drop = (size_t)keepFrom * (size_t)ch_;
        if (drop >= src_.size()) { src_.clear(); pos_ = 0.0; return; }
        src_.erase(src_.begin(), src_.begin() + (ptrdiff_t)drop);
        pos_ -= (double)keepFrom;
    }

    std::vector<float> src_;
    double pos_ = 0.0;
    int    ch_  = 0;
};
