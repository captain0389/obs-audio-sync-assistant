#pragma once

#include <obs.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

class AudioCaptureBuffer {
public:
    void clear();
    void start();
    void stop();
    void push(const struct audio_data *data);
    std::vector<float> snapshot(size_t max_samples) const;
    bool active() const { return active_.load(); }

private:
    mutable std::mutex mutex_;
    std::vector<float> samples_;
    std::atomic<bool> active_{false};
    size_t max_samples_ = 48000 * 12;
};

// Captures one OBS post-mix audio track. Unlike a source audio callback,
// this receives audio after OBS has applied the source's sync offset and
// routed the source into the selected mixer track.
class RawMixAudioTap {
public:
    RawMixAudioTap() = default;
    ~RawMixAudioTap();

    bool attach(size_t mix_idx, AudioCaptureBuffer *buffer, uint32_t sample_rate = 48000);
    void detach();
    size_t mix_idx() const { return mix_idx_; }

private:
    static void on_audio(void *param, size_t mix_idx, struct audio_data *data);

    size_t mix_idx_ = 0;
    AudioCaptureBuffer *buffer_ = nullptr;
    bool attached_ = false;
};
