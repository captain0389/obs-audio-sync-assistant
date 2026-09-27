#pragma once

#include <obs.h>

#include <vector>

namespace calibration_source {

constexpr const char *SOURCE_ID = "obs_audio_sync_calibration";

void register_source();
obs_source_t *create_private();
void trigger(obs_source_t *source);
void cancel(obs_source_t *source);

// Returns the exact calibration waveform emitted by the source, at 48 kHz mono.
// The waveform does not include the continuously emitted silence before/after it.
const std::vector<float> &signal();

} // namespace calibration_source
