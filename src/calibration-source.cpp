#include "calibration-source.hpp"

#include <obs.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr uint32_t SAMPLE_RATE = 48000;
constexpr uint32_t BLOCK_FRAMES = 480; // 10 ms
constexpr uint32_t LEAD_SILENCE_FRAMES = SAMPLE_RATE / 4;
constexpr double PI = 3.1415926535897932384626433832795;

struct CalibrationData {
    obs_source_t *source = nullptr;
    std::thread thread;
    std::mutex mutex;
    std::condition_variable cv;
    bool stop = false;
    bool triggered = false;
};

std::mutex registry_mutex;
std::unordered_map<obs_source_t *, CalibrationData *> registry;

std::vector<float> make_signal()
{
    // A distinctive two-part logarithmic sweep with a short gap.  This is
    // intentionally much easier to identify through a microphone than
    // arbitrary desktop audio.
    constexpr size_t sweep_frames = SAMPLE_RATE * 3 / 10; // 300 ms
    constexpr size_t gap_frames = SAMPLE_RATE * 15 / 100; // 150 ms

    std::vector<float> out;
    out.reserve(sweep_frames + gap_frames + sweep_frames);

    auto append_sweep = [&](double start_hz, double end_hz) {
        constexpr double amplitude = 0.40; // approximately -8 dBFS, still well below digital clipping
        constexpr double fade_ms = 5.0;
        const size_t fade_frames =
            static_cast<size_t>(SAMPLE_RATE * fade_ms / 1000.0);

        const double duration =
            static_cast<double>(sweep_frames) / SAMPLE_RATE;
        const double ratio = end_hz / start_hz;

        for (size_t i = 0; i < sweep_frames; ++i) {
            const double t = static_cast<double>(i) / SAMPLE_RATE;
            const double phase =
                2.0 * PI * start_hz * duration / std::log(ratio) *
                (std::pow(ratio, t / duration) - 1.0);

            double envelope = 1.0;
            if (i < fade_frames)
                envelope = static_cast<double>(i) / fade_frames;
            else if (i + fade_frames > sweep_frames)
                envelope =
                    static_cast<double>(sweep_frames - i) / fade_frames;

            out.push_back(static_cast<float>(
                std::sin(phase) * amplitude * envelope));
        }
    };

    append_sweep(350.0, 6500.0);
    out.insert(out.end(), gap_frames, 0.0f);
    append_sweep(6500.0, 350.0);

    return out;
}

const std::vector<float> &signal_impl()
{
    static const std::vector<float> signal = make_signal();
    return signal;
}

void output_block(CalibrationData *data, const float *samples,
                  uint32_t frames, uint64_t timestamp)
{
    struct obs_source_audio audio {};
    audio.data[0] =
        reinterpret_cast<const uint8_t *>(samples);
    audio.frames = frames;
    audio.speakers = SPEAKERS_MONO;
    audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
    audio.samples_per_sec = SAMPLE_RATE;
    audio.timestamp = timestamp;
    obs_source_output_audio(data->source, &audio);
}

void thread_main(CalibrationData *data)
{
    std::vector<float> silence(BLOCK_FRAMES, 0.0f);
    const auto &signal = signal_impl();

    uint64_t timestamp = 0;
    size_t signal_pos = 0;
    size_t lead_remaining = 0;
    bool playing = false;

    while (true) {
        {
            std::unique_lock lock(data->mutex);
            if (!data->triggered && !data->stop) {
                data->cv.wait_for(
                    lock, std::chrono::milliseconds(10), [&] {
                        return data->triggered || data->stop;
                    });
            }

            if (data->stop)
                break;

            if (data->triggered && !playing) {
                data->triggered = false;
                playing = true;
                signal_pos = 0;
                lead_remaining = LEAD_SILENCE_FRAMES;
            }
        }

        const float *block = silence.data();
        std::vector<float> generated;

        if (playing) {
            generated.resize(BLOCK_FRAMES, 0.0f);

            for (uint32_t i = 0; i < BLOCK_FRAMES; ++i) {
                if (lead_remaining > 0) {
                    --lead_remaining;
                    continue;
                }

                if (signal_pos < signal.size())
                    generated[i] = signal[signal_pos++];
            }

            block = generated.data();

            if (signal_pos >= signal.size() && lead_remaining == 0)
                playing = false;
        }

        output_block(data, block, BLOCK_FRAMES, timestamp);
        timestamp +=
            static_cast<uint64_t>(BLOCK_FRAMES) * 1000000000ULL /
            SAMPLE_RATE;

        // Keep output on a real-time 10 ms cadence.  The OBS test sine source
        // follows the same model; timestamps alone should not be used to
        // flood the audio pipeline with future samples.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

const char *get_name(void *)
{
    return "Audio Sync Calibration Signal";
}

void *create(obs_data_t *, obs_source_t *source)
{
    auto *data = new CalibrationData();
    data->source = source;

    {
        std::lock_guard lock(registry_mutex);
        registry[source] = data;
    }

    data->thread = std::thread(thread_main, data);
    return data;
}

void destroy(void *opaque)
{
    auto *data = static_cast<CalibrationData *>(opaque);
    if (!data)
        return;

    {
        std::lock_guard lock(registry_mutex);
        registry.erase(data->source);
    }

    {
        std::lock_guard lock(data->mutex);
        data->stop = true;
        data->triggered = false;
    }
    data->cv.notify_all();

    if (data->thread.joinable())
        data->thread.join();

    delete data;
}

struct obs_source_info source_info = {
    .id = calibration_source::SOURCE_ID,
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_AUDIO,
    .get_name = get_name,
    .create = create,
    .destroy = destroy,
};

} // namespace

namespace calibration_source {

void register_source()
{
    obs_register_source(&source_info);
}

obs_source_t *create_private()
{
    return obs_source_create_private(SOURCE_ID,
                                     "Audio Sync Calibration", nullptr);
}

void trigger(obs_source_t *source)
{
    CalibrationData *data = nullptr;
    {
        std::lock_guard lock(registry_mutex);
        const auto it = registry.find(source);
        if (it != registry.end())
            data = it->second;
    }

    if (!data)
        return;

    {
        std::lock_guard lock(data->mutex);
        if (!data->stop)
            data->triggered = true;
    }
    data->cv.notify_all();
}

void cancel(obs_source_t *source)
{
    CalibrationData *data = nullptr;
    {
        std::lock_guard lock(registry_mutex);
        const auto it = registry.find(source);
        if (it != registry.end())
            data = it->second;
    }

    if (!data)
        return;

    std::lock_guard lock(data->mutex);
    data->triggered = false;
}

const std::vector<float> &signal()
{
    return signal_impl();
}

} // namespace calibration_source
