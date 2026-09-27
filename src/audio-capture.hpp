#pragma once

#include <obs.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
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

class SourceAudioTap {
public:
    SourceAudioTap() = default;
    ~SourceAudioTap();

    bool attach(obs_source_t *source, AudioCaptureBuffer *buffer);
    void detach();
    obs_source_t *source() const { return source_; }

private:
    static void on_audio(void *param, obs_source_t *source, const struct audio_data *data, bool muted);
    obs_source_t *source_ = nullptr;
    AudioCaptureBuffer *buffer_ = nullptr;
};
