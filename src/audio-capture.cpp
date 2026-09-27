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

    // OBS source capture audio is float planar. Mix available planes to mono.
    int channels = 0;
    while (channels < MAX_AV_PLANES && data->data[channels])
        ++channels;
    if (channels <= 0)
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
    for (size_t i = 0; i < needed; ++i) {
        double sum = 0.0;
        int valid = 0;
        for (int ch = 0; ch < channels; ++ch) {
            const float *plane = reinterpret_cast<const float *>(data->data[ch]);
            if (plane) {
                sum += plane[i];
                ++valid;
            }
        }
        samples_.push_back(valid ? static_cast<float>(sum / valid) : 0.0f);
    }
}

std::vector<float> AudioCaptureBuffer::snapshot(size_t max_samples) const
{
    std::lock_guard lock(mutex_);
    const size_t count = std::min(max_samples, samples_.size());
    return std::vector<float>(samples_.end() - static_cast<std::ptrdiff_t>(count), samples_.end());
}

SourceAudioTap::~SourceAudioTap()
{
    detach();
}

bool SourceAudioTap::attach(obs_source_t *source, AudioCaptureBuffer *buffer)
{
    detach();
    if (!source || !buffer)
        return false;

    source_ = obs_source_get_ref(source);
    buffer_ = buffer;
    obs_source_add_audio_capture_callback(source_, on_audio, this);
    return true;
}

void SourceAudioTap::detach()
{
    if (source_) {
        obs_source_remove_audio_capture_callback(source_, on_audio, this);
        obs_source_release(source_);
        source_ = nullptr;
    }
    buffer_ = nullptr;
}

void SourceAudioTap::on_audio(void *param, obs_source_t *, const struct audio_data *data, bool muted)
{
    auto *tap = static_cast<SourceAudioTap *>(param);
    if (!tap || muted || !tap->buffer_)
        return;
    tap->buffer_->push(data);
}
