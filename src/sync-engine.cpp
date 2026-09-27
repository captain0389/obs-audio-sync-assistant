#include "sync-engine.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numeric>
#include <string>
#include <vector>

namespace {
using cd = std::complex<double>;
constexpr double PI = 3.1415926535897932384626433832795;

void fft(std::vector<cd> &a, bool inverse)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }

    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = 2.0 * PI / static_cast<double>(len) * (inverse ? 1.0 : -1.0);
        const cd wlen(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            cd w(1.0, 0.0);
            for (size_t j = 0; j < len / 2; ++j) {
                const cd u = a[i + j];
                const cd v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }

    if (inverse) {
        const double inv_n = 1.0 / static_cast<double>(n);
        for (cd &v : a)
            v *= inv_n;
    }
}

size_t next_pow2(size_t x)
{
    size_t n = 1;
    while (n < x)
        n <<= 1;
    return n;
}

void preprocess(std::vector<float> &x)
{
    if (x.empty())
        return;
    const double mean = std::accumulate(x.begin(), x.end(), 0.0) / x.size();
    double energy = 0.0;
    for (float &v : x) {
        v = static_cast<float>(v - mean);
        energy += static_cast<double>(v) * v;
    }
    const double rms = std::sqrt(energy / x.size());
    if (rms > 1e-8) {
        for (float &v : x)
            v = static_cast<float>(v / rms);
    }
}
}

SyncResult estimate_sync_gcc_phat(const std::vector<float> &reference,
                                  const std::vector<float> &target,
                                  uint32_t sample_rate,
                                  int max_offset_ms)
{
    SyncResult out;
    out.sample_rate = sample_rate;
    out.samples_used = std::min(reference.size(), target.size());

    if (sample_rate == 0 || out.samples_used < sample_rate / 2) {
        out.message = "Not enough audio. Capture at least 0.5 seconds.";
        return out;
    }

    const size_t usable = static_cast<size_t>(out.samples_used);
    const size_t n = next_pow2(usable * 2);
    std::vector<cd> A(n), B(n);
    for (size_t i = 0; i < usable; ++i) {
        A[i] = reference[i];
        B[i] = target[i];
    }
    std::vector<float> ref(reference.begin(), reference.begin() + usable);
    std::vector<float> tgt(target.begin(), target.begin() + usable);
    preprocess(ref);
    preprocess(tgt);
    for (size_t i = 0; i < usable; ++i) {
        A[i] = ref[i];
        B[i] = tgt[i];
    }

    fft(A, false);
    fft(B, false);
    for (size_t k = 0; k < n; ++k) {
        const cd cross = A[k] * std::conj(B[k]);
        const double mag = std::abs(cross);
        A[k] = mag > 1e-12 ? cross / mag : cd(0.0, 0.0);
    }
    fft(A, true);

    const int max_lag = std::min<int>(max_offset_ms * static_cast<int>(sample_rate) / 1000,
                                      static_cast<int>(usable) - 1);
    double best = -1.0;
    int best_lag = 0;

    for (int lag = -max_lag; lag <= max_lag; ++lag) {
        const size_t idx = lag >= 0 ? static_cast<size_t>(lag) : n - static_cast<size_t>(-lag);
        const double score = std::abs(A[idx]);
        if (score > best) {
            best = score;
            best_lag = lag;
        }
    }

    // A real correlation peak spans multiple neighboring samples. Exclude a
    // small neighborhood around the winning peak before looking for a second
    // competing peak; otherwise the peak's natural width is mistaken for
    // ambiguity.
    const int exclusion = std::max(1, static_cast<int>(sample_rate * 0.008));
    double second = -1.0;
    for (int lag = -max_lag; lag <= max_lag; ++lag) {
        if (std::abs(lag - best_lag) <= exclusion)
            continue;
        const size_t idx = lag >= 0 ? static_cast<size_t>(lag) : n - static_cast<size_t>(-lag);
        second = std::max(second, std::abs(A[idx]));
    }

    // Refine the peak to sub-sample precision with a 3-point parabolic fit.
    double delta = 0.0;
    const auto corr_at = [&](int lag) -> double {
        const size_t idx = lag >= 0 ? static_cast<size_t>(lag) : n - static_cast<size_t>(-lag);
        return std::abs(A[idx]);
    };
    if (best_lag > -max_lag && best_lag < max_lag) {
        const double ym = corr_at(best_lag - 1);
        const double y0 = corr_at(best_lag);
        const double yp = corr_at(best_lag + 1);
        const double denom = ym - 2.0 * y0 + yp;
        if (std::abs(denom) > 1e-12)
            delta = 0.5 * (ym - yp) / denom;
    }

    const double lag_samples = static_cast<double>(best_lag) + delta;
    out.offset_ms = lag_samples * 1000.0 / static_cast<double>(sample_rate);
    out.peak_ratio = second > 1e-9 ? best / second : 99.0;

    // Confidence is intentionally conservative. A strong isolated peak gets
    // near 1.0; ambiguous peaks stay visibly lower.
    out.confidence = std::clamp((out.peak_ratio - 1.02) / 0.25, 0.0, 1.0);
    out.valid = best > 0.05 && out.confidence >= 0.25;
    out.message = out.valid ? "Measurement complete." : "The correlation peak is ambiguous. Try a louder/cleaner signal.";
    return out;
}
