#include "audio-capture.hpp"

#include <algorithm>

void AudioCaptureBuffer::clear()
{
    std::lock_guard lock(mutex_);
    samples_.clear();
}

void AudioCaptureBuffer::start()
{
    clear();
    active_.store(true);
}

void AudioCaptureBuffer::stop()
{
    active_.store(false);
}

void AudioCaptureBuffer::push(const struct audio_data *data)
{
    if (!data || !active_.load() || data->frames == 0)
        return;

    // Raw-mix capture requests mono float planar conversion, so the first
    // plane contains the complete measurement signal.
    const float *plane = reinterpret_cast<const float *>(data->data[0]);
    if (!plane)
        return;

    std::lock_guard lock(mutex_);
    const size_t needed = std::min<size_t>(data->frames, max_samples_);
    if (samples_.size() + needed > max_samples_) {
        const size_t remove = samples_.size() + needed - max_samples_;
        if (remove >= samples_.size())
            samples_.clear();
        else
            samples_.erase(samples_.begin(), samples_.begin() + static_cast<std::ptrdiff_t>(remove));
    }

    samples_.reserve(std::min(max_samples_, samples_.size() + needed));
    samples_.insert(samples_.end(), plane, plane + needed);
}

std::vector<float> AudioCaptureBuffer::snapshot(size_t max_samples) const
{
    std::lock_guard lock(mutex_);
    const size_t count = std::min(max_samples, samples_.size());
    return std::vector<float>(samples_.end() - static_cast<std::ptrdiff_t>(count), samples_.end());
}

RawMixAudioTap::~RawMixAudioTap()
{
    detach();
}

bool RawMixAudioTap::attach(size_t mix_idx, AudioCaptureBuffer *buffer, uint32_t sample_rate)
{
    detach();
    if (!buffer || sample_rate == 0)
        return false;

    audio_convert_info conversion{};
    conversion.samples_per_sec = sample_rate;
    conversion.format = AUDIO_FORMAT_FLOAT_PLANAR;
    conversion.speakers = SPEAKERS_MONO;

    mix_idx_ = mix_idx;
    buffer_ = buffer;
    obs_add_raw_audio_callback(mix_idx_, &conversion, on_audio, this);
    attached_ = true;
    return true;
}

void RawMixAudioTap::detach()
{
    if (attached_) {
        obs_remove_raw_audio_callback(mix_idx_, on_audio, this);
        attached_ = false;
    }
    buffer_ = nullptr;
}

void RawMixAudioTap::on_audio(void *param, size_t, struct audio_data *data)
{
    auto *tap = static_cast<RawMixAudioTap *>(param);
    if (!tap || !tap->buffer_)
        return;
    tap->buffer_->push(data);
}
