#pragma once

#include <cstdint>
#include <vector>

struct SyncResult {
    bool valid = false;
    double offset_ms = 0.0;
    double confidence = 0.0;
    double peak_ratio = 0.0;
    uint32_t sample_rate = 48000;
    uint64_t samples_used = 0;
    const char *message = nullptr;
};

// Convention: positive offset means the target source LEADS the reference,
// so delaying the target by +offset_ms aligns it to the reference.
SyncResult estimate_sync_gcc_phat(const std::vector<float> &reference,
                                  const std::vector<float> &target,
                                  uint32_t sample_rate,
                                  int max_offset_ms = 500);
